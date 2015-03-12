#include <errno.h>
#include <string.h>

#include <gpio.h>
#include <spi.h>
#include <spi_priv.h>
#include <util.h>

#include <plat/inc/cmsis.h>
#include <plat/inc/gpio.h>
#include <plat/inc/pwr.h>

#define SPI_CR1_CPHA                (1 << 0)
#define SPI_CR1_CPOL                (1 << 1)
#define SPI_CR1_MSTR                (1 << 2)

#define SPI_CR1_BR(x)               ((LOG2_CEIL(x) - 1) << 3)
#define SPI_CR1_BR_MIN              2
#define SPI_CR1_BR_MAX              256
#define SPI_CR1_BR_MASK             (0x7 << 3)

#define SPI_CR1_SPE                 (1 << 6)
#define SPI_CR1_LSBFIRST            (1 << 7)
#define SPI_CR1_RXONLY              (1 << 10)
#define SPI_CR1_DFF                 (1 << 11)
#define SPI_CR1_BIDIOE              (1 << 14)
#define SPI_CR1_BIDIMODE            (1 << 15)

#define SPI_CR2_TXEIE               (1 << 7)
#define SPI_CR2_RXNEIE              (1 << 6)
#define SPI_CR2_ERRIE               (1 << 5)
#define SPI_CR2_INT_MASK            (SPI_CR2_TXEIE | SPI_CR2_RXNEIE | SPI_CR2_ERRIE)

#define SPI_CR2_SSOE                (1 << 2)

#define SPI_SR_RXNE                 (1 << 0)
#define SPI_SR_TXE                  (1 << 1)
#define SPI_SR_BSY                  (1 << 7)

struct StmSpi {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t CRCPR;
    volatile uint32_t RXCRCR;
    volatile uint32_t TXCRCR;
    volatile uint32_t I2SCFGR;
    volatile uint32_t I2SPR;
};

struct StmSpiState {
    uint8_t bitsPerWord;
    uint16_t words;

    void *rxBuf;
    uint16_t rxIdx;

    const void *txBuf;
    uint16_t txIdx;
};

struct StmSpiCfg {
    struct StmSpi *regs;

    uint32_t clokcBus;
    uint32_t clockUnit;

    GpioNum gpioMiso;
    GpioNum gpioMosi;
    GpioNum gpioSclk;
    GpioNum gpioNss;
    uint8_t gpioFunc;

    IRQn_Type irq;
};

struct StmSpiDev {
    struct SpiDevice *base;
    const struct StmSpiCfg cfg;
    struct StmSpiState state;

    struct Gpio miso;
    struct Gpio mosi;
    struct Gpio sck;
    struct Gpio nss;
};

static int stmSpiMasterStartSync(struct SpiDevice *dev, spi_cs_t cs,
        const struct SpiMode *mode)
{
    struct StmSpiDev *pdev = dev->pdata;
    struct StmSpi *regs = pdev->cfg.regs;

    if (!mode->speed)
        return -EINVAL;

    if (mode->bitsPerWord != 8 &&
            mode->bitsPerWord != 16)
        return -EINVAL;

    uint32_t pclk = pwrGetBusSpeed(PERIPH_BUS_AHB1);
    unsigned int div = pclk / mode->speed;
    if (div > SPI_CR1_BR_MAX)
        return -EINVAL;
    else if (div < SPI_CR1_BR_MIN)
        div = SPI_CR1_BR_MIN;

    pwrUnitClock(pdev->cfg.clokcBus, pdev->cfg.clockUnit, true);

    regs->CR1 &= ~SPI_CR1_BR_MASK;
    regs->CR1 |= SPI_CR1_BR(div);

    if (mode->cpol == SPI_CPOL_IDLE_LO)
        regs->CR1 &= ~SPI_CR1_CPOL;
    else
        regs->CR1 |= SPI_CR1_CPOL;

    if (mode->cpha == SPI_CPHA_LEADING_EDGE)
        regs->CR1 &= ~SPI_CR1_CPHA;
    else
        regs->CR1 |= SPI_CR1_CPHA;

    if (mode->bitsPerWord == 8)
        regs->CR1 &= ~SPI_CR1_DFF;
    else
        regs->CR1 |= SPI_CR1_DFF;

    if (mode->format == SPI_FORMAT_MSB_FIRST)
        regs->CR1 &= ~SPI_CR1_LSBFIRST;
    else
        regs->CR1 |= SPI_CR1_LSBFIRST;

    regs->CR2 |= SPI_CR2_SSOE;
    regs->CR1 |= SPI_CR1_MSTR;

    return 0;
}

static void stmSpiTxNExtByte(struct StmSpiDev *pdev)
{
    struct StmSpi *regs = pdev->cfg.regs;
    struct StmSpiState *state = &pdev->state;

    if (state->bitsPerWord == 8) {
        const uint8_t *txBuf8 = state->txBuf;
        regs->DR = txBuf8[state->txIdx];
    } else {
        const uint16_t *txBuf16 = state->txBuf;
        regs->DR = txBuf16[state->txIdx];
    }
}

static int stmSpiMasterRxTx(struct SpiDevice *dev, void *rxBuf,
        const void *txBuf, size_t size, const struct SpiMode *mode)
{
    struct StmSpiDev *pdev = dev->pdata;
    struct StmSpi *regs = pdev->cfg.regs;
    struct StmSpiState *state = &pdev->state;

    state->bitsPerWord = mode->bitsPerWord;
    state->rxBuf = rxBuf;
    state->rxIdx = 0;
    state->txBuf = txBuf;
    state->txIdx = 0;

    if (mode->bitsPerWord == 8)
        state->words = size;
    else
        state->words = size / 2;

    regs->CR2 &= ~SPI_CR2_INT_MASK;
    regs->CR2 |= SPI_CR2_ERRIE;

    if (rxBuf)
        regs->CR2 |= SPI_CR2_RXNEIE;

    if (txBuf) {
        regs->CR1 &= ~SPI_CR1_RXONLY;
        regs->CR2 |= SPI_CR2_TXEIE;
        stmSpiTxNExtByte(pdev);
    } else {
        regs->CR1 |= SPI_CR1_RXONLY;
    }

    regs->CR1 |= SPI_CR1_SPE;
    return 0;
}

static int stmSpiMasterStopSync(struct SpiDevice *dev)
{
    struct StmSpiDev *pdev = dev->pdata;

    pwrUnitClock(pdev->cfg.clokcBus, pdev->cfg.clockUnit, false);
    return 0;
}

static void stmSpiDone(struct StmSpiDev *pdev)
{
    struct StmSpi *regs = pdev->cfg.regs;
    struct StmSpiState *state = &pdev->state;

    if (state->txBuf)
        while (regs->SR & SPI_SR_BSY)
            ;

    regs->CR1 &= ~SPI_CR1_SPE;
    spiMasterRxTxDone(pdev->base, 0);
}

static void stmSpiRxDone(struct StmSpiDev *pdev)
{
    struct StmSpi *regs = pdev->cfg.regs;

    regs->CR2 &= ~SPI_CR2_RXNEIE;
    stmSpiDone(pdev);
}

static void stmSpiTxDone(struct StmSpiDev *pdev)
{
    struct StmSpi *regs = pdev->cfg.regs;
    struct StmSpiState *state = &pdev->state;

    regs->CR2 &= ~SPI_CR2_TXEIE;
    if (!state->rxBuf)
        stmSpiDone(pdev);
}

static void stmSpiTxe(struct StmSpiDev *pdev)
{
    struct StmSpiState *state = &pdev->state;

    state->txIdx++;
    if (state->txIdx == state->words)
        stmSpiTxDone(pdev);
    else
        stmSpiTxNExtByte(pdev);
}

static void stmSpiRxne(struct StmSpiDev *pdev)
{
    struct StmSpi *regs = pdev->cfg.regs;
    struct StmSpiState *state = &pdev->state;

    if (state->bitsPerWord == 8) {
        uint8_t *rxBuf8 = state->rxBuf;
        rxBuf8[state->rxIdx] = regs->DR;
    } else {
        uint16_t *rxBuf16 = state->rxBuf;
        rxBuf16[state->rxIdx] = regs->DR;
    }

    state->rxIdx++;
    if (state->rxIdx == state->words)
        stmSpiRxDone(pdev);
}

static void stmSpiIsr(struct StmSpiDev *pdev)
{
    struct StmSpi *regs = pdev->cfg.regs;

    if (regs->SR & SPI_SR_RXNE) {
        stmSpiRxne(pdev);
    } else if (regs->SR & SPI_SR_TXE) {
        stmSpiTxe(pdev);
    } else {
        /* TODO */
    }
}

#define DECLARE_IRQ_HANDLER(_n)             \
    void SPI##_n##_IRQHandler();            \
    void SPI##_n##_IRQHandler()             \
    {                                       \
        stmSpiIsr(&mStmSpiDevs[_n - 1]); \
    }

const struct SpiDevice_ops mStmSpiOps = {
    .masterStartSync = stmSpiMasterStartSync,
    .masterRxTx = stmSpiMasterRxTx,
    .masterStopSync = stmSpiMasterStopSync,
};

static struct StmSpiDev mStmSpiDevs[] = {
    [0] = {
        .cfg = {
            .regs = (struct StmSpi *)SPI1_BASE,

            .clokcBus = PERIPH_BUS_APB2,
            .clockUnit = PERIPH_APB2_SPI1,

            .gpioMiso = GPIO_PA(6),
            .gpioMosi = GPIO_PA(7),
            .gpioSclk = GPIO_PA(5),
            .gpioNss = GPIO_PA(4),
            .gpioFunc = GPIO_A2_AFR_SPI123,

            .irq = SPI1_IRQn,
        },
    },
};
DECLARE_IRQ_HANDLER(1)

static inline void stmSpiGpioInit(struct Gpio *gpio,
        GpioNum number, uint8_t func)
{
    gpioRequest(gpio, number);
    gpioConfig(gpio, GPIO_MODE_ALTERNATE, GPIO_PULL_NONE);
    gpio_assign_func(gpio, func);
}

static void stmSpiInit(struct StmSpiDev *pdev, struct SpiDevice *dev)
{
    const struct StmSpiCfg *cfg = &pdev->cfg;

    stmSpiGpioInit(&pdev->miso, cfg->gpioMiso, cfg->gpioFunc);
    stmSpiGpioInit(&pdev->mosi, cfg->gpioMosi, cfg->gpioFunc);
    stmSpiGpioInit(&pdev->sck, cfg->gpioSclk, cfg->gpioFunc);
    stmSpiGpioInit(&pdev->nss, cfg->gpioNss, cfg->gpioFunc);

    NVIC_EnableIRQ(cfg->irq);

    pdev->base = dev;
}

int spiRequest(struct SpiDevice *dev, uint8_t busId)
{
    if (busId >= ARRAY_SIZE(mStmSpiDevs))
        return -ENODEV;

    struct StmSpiDev *pdev = &mStmSpiDevs[busId];
    if (!pdev->base)
        stmSpiInit(&mStmSpiDevs[busId], dev);

    memset(&pdev->state, 0, sizeof(pdev->state));
    dev->ops = &mStmSpiOps;
    dev->pdata = pdev;
    return 0;
}
