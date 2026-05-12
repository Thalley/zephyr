/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Driver for the Cirrus Logic CS47L63 Low-Power Audio DSP.
 *
 * The CS47L63 is a member of the Cirrus Logic Madera codec family. It
 * communicates via:
 *   - SPI for register access (control interface)
 *   - I2S for audio data (data interface, driven via a referenced I2S device)
 *
 * SPI frame format (8 bytes per transaction):
 *   bytes 0-3: 32-bit address word, big-endian
 *       bits [31:1] = register address
 *       bit  [0]   = 0 for write, 1 for read
 *   bytes 4-7: 32-bit data word, big-endian
 *       bits [31:16] = 0 (reserved/don't-care)
 *       bits [15:0]  = register value
 *
 * References:
 *   CS47L63 Product Datasheet, Cirrus Logic
 *   Linux kernel Madera platform driver:
 *     sound/soc/codecs/cs47l63.c
 *     include/linux/mfd/madera/registers.h
 *     sound/soc/codecs/madera.c
 */

#define DT_DRV_COMPAT cirrus_cs47l63

#include <errno.h>
#include <string.h>

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(cs47l63, CONFIG_AUDIO_CODEC_LOG_LEVEL);

/* -----------------------------------------------------------------------
 * CS47L63 / Madera register addresses
 * ----------------------------------------------------------------------- */

/* Device ID (read) / software reset (write) */
#define CS47L63_DEVID                   0x0000U
/* Expected lower-16-bit device ID for CS47L63 */
#define CS47L63_DEVID_VAL               0xA463U

/* Chip revision */
#define CS47L63_REVID                   0x0001U

/* System clock */
#define CS47L63_SYSTEM_CLOCK_1          0x0101U

/* Sample rate */
#define CS47L63_SAMPLE_RATE_1           0x0113U

/* Output path sample rate */
#define CS47L63_OUTPUT_RATE_1           0x0181U

/* Output enables */
#define CS47L63_OUTPUT_ENABLES_1        0x0418U

/* DAC digital volume for headphone channels */
#define CS47L63_DAC_DIGITAL_VOLUME_1L   0x0415U
#define CS47L63_DAC_DIGITAL_VOLUME_1R   0x0417U

/* AIF1 – serial audio interface connected to the nRF5340 I2S bus */
#define CS47L63_AIF1_BCLK_CTRL         0x0480U
#define CS47L63_AIF1_FORMAT            0x0486U
#define CS47L63_AIF1_FRAME_CTRL_1     0x0488U
#define CS47L63_AIF1_FRAME_CTRL_2     0x0489U
#define CS47L63_AIF1_ENABLES_1        0x04A0U

/* Mixer input routing: OUT1L and OUT1R */
#define CS47L63_OUT1LMIX_INPUT_1_SOURCE  0x0680U
#define CS47L63_OUT1LMIX_INPUT_1_VOLUME  0x0681U
#define CS47L63_OUT1RMIX_INPUT_1_SOURCE  0x0688U
#define CS47L63_OUT1RMIX_INPUT_1_VOLUME  0x0689U

/* -----------------------------------------------------------------------
 * Register field values
 * ----------------------------------------------------------------------- */

/*
 * SYSTEM_CLOCK_1 (0x0101)
 *
 * Bit layout (from Madera platform Linux driver):
 *   bit 23     : SYSCLK_FRAC — 0 = integer, 1 = fractional
 *   bits [22:20]: SYSCLK_FREQ — index into the frequency table below
 *   bit  6     : SYSCLK_ENA  — 1 = enable the system clock
 *   bits [3:0] : SYSCLK_SRC  — clock source
 *
 * Frequency table (SYSCLK_FREQ index → Hz):
 *   0 = 5.6448 MHz,  1 = 11.2896 MHz,  2 = 22.5792 MHz,
 *   3 = 45.1584 MHz, 4 = 90.3168 MHz,
 *   5 = 6.144 MHz,   6 = 12.288 MHz,   7 = 24.576 MHz,
 *   8 = 49.152 MHz,  9 = 98.304 MHz
 *
 * The nRF5340 Audio DK routes the I2S MCK output (12.288 MHz) to the
 * CS47L63 MCLK1 pin.  MCLK1 source code = 0x4.
 */
#define CS47L63_SYSCLK_FREQ_SHIFT       20U
#define CS47L63_SYSCLK_FREQ_12MHZ       6U   /* index for 12.288 MHz */
#define CS47L63_SYSCLK_ENA              BIT(6)
#define CS47L63_SYSCLK_SRC_MCLK1       0x04U

/*
 * SAMPLE_RATE_1 / OUTPUT_RATE_1 — bits [3:0]
 * Values from madera-core.c (madera_sr_vals[]):
 */
#define CS47L63_SR_8K   0x01U
#define CS47L63_SR_16K  0x04U
#define CS47L63_SR_32K  0x07U
#define CS47L63_SR_44K1 0x08U
#define CS47L63_SR_48K  0x09U
#define CS47L63_SR_96K  0x0BU

/*
 * AIF1_FORMAT (0x0486) — bits [2:0]
 *   0 = left-justified, 1 = I2S, 3 = DSP-A, 4 = DSP-B
 */
#define CS47L63_AIF_FMT_I2S     0x01U

/*
 * AIF1_BCLK_CTRL (0x0480)
 *   bit 4: AIF1_BCLK_MSTR — 0 = slave (nRF5340 is I2S master), 1 = master
 */
#define CS47L63_AIF_BCLK_SLAVE  0x0000U

/*
 * AIF1_FRAME_CTRL_1 (0x0488) — bits [4:0]: word length
 *   0x0F = 16-bit, 0x17 = 24-bit, 0x1F = 32-bit
 */
#define CS47L63_AIF_WL_16       0x0FU
#define CS47L63_AIF_WL_24       0x17U
#define CS47L63_AIF_WL_32       0x1FU

/*
 * AIF1_ENABLES_1 (0x04A0)
 *   bit 0: AIF1_RX_ENA — enable AIF1 receive (CS47L63 receives from nRF5340)
 *   bit 1: AIF1_TX_ENA — enable AIF1 transmit
 */
#define CS47L63_AIF_RX_ENA      BIT(0)

/*
 * OUTPUT_ENABLES_1 (0x0418)
 *   bit 0: OUT1L_ENA, bit 1: OUT1R_ENA
 */
#define CS47L63_OUT1L_ENA       BIT(0)
#define CS47L63_OUT1R_ENA       BIT(1)

/*
 * DAC_DIGITAL_VOLUME_1L/R
 *   bit  9  : MUTE — 1 = muted
 *   bits [8:1]: volume field
 *     0x00 = −64 dB, 0xBF = 0 dB, 0xFF = +31.5 dB  (0.5 dB/step)
 * 0 dB not muted: volume_field = 0xBF at bits [8:1] → word = 0xBF << 1 = 0x017E
 */
#define CS47L63_DAC_MUTE        BIT(9)
#define CS47L63_DAC_VOL_SHIFT   1U
#define CS47L63_DAC_VOL_0DB     0xBFU  /* field value before shift */
#define CS47L63_DAC_VOL_MINDB   0x80U  /* ~−32 dB */

/*
 * Mixer source IDs (written to OUTxMIX_INPUT_x_SOURCE):
 *   0x60 = AIF1 RX1 (left channel from the I2S stream)
 *   0x61 = AIF1 RX2 (right channel from the I2S stream)
 */
#define CS47L63_SRC_AIF1RX1     0x60U
#define CS47L63_SRC_AIF1RX2     0x61U

/* Mixer input volume: 0 dB applied to the selected source. */
#define CS47L63_MIX_VOL_0DB     0x80U

/* -----------------------------------------------------------------------
 * I2S streaming constants
 * ----------------------------------------------------------------------- */

/*
 * Maximum mono block size fed by the application (bytes).
 * Matches CODEC_BLOCK_SIZE in the bap_broadcast_sink hw_codec.c.
 */
#define CS47L63_MONO_BLOCK_SIZE         480U

/*
 * DMA block size on the I2S bus: always stereo 16-bit regardless of the
 * application's channel count.  Mono input is duplicated to both channels.
 */
#define CS47L63_I2S_BLOCK_SIZE          (2U * CS47L63_MONO_BLOCK_SIZE)

/* Number of DMA blocks in the I2S memory slab (double-buffering + headroom). */
#define CS47L63_TX_SLAB_BLOCKS          4U

/* TX thread stack size (bytes). */
#define CS47L63_TX_STACK_SIZE CONFIG_AUDIO_CODEC_CS47L63_TX_STACK_SIZE

/* I2S write timeout in milliseconds. */
#define CS47L63_I2S_TIMEOUT_MS          2000

/* -----------------------------------------------------------------------
 * Driver structures
 * ----------------------------------------------------------------------- */

struct cs47l63_config {
	struct spi_dt_spec  spi;
	struct gpio_dt_spec reset_gpio;
	const struct device *i2s_dev;
	k_thread_stack_t    *tx_stack;
};

struct cs47l63_data {
	/* TX-done callback registered by the application */
	audio_codec_tx_done_callback_t tx_done_cb;
	void                          *tx_done_user_data;

	/* PCM parameters saved during configure() */
	uint32_t sample_rate;
	uint8_t  channels;
	uint8_t  word_size;
	size_t   block_size;

	/* I2S memory slab (statically sized; large enough for 16-bit stereo) */
	struct k_mem_slab tx_slab;
	uint8_t           tx_slab_buf[CS47L63_TX_SLAB_BLOCKS *
				       CS47L63_I2S_BLOCK_SIZE] __aligned(4);

	/*
	 * Block currently being prepared for write().
	 * Set by the TX thread before calling tx_done_cb; cleared by write().
	 */
	void *current_block;

	/* TX streaming thread handle */
	struct k_thread tx_thread;
	volatile bool   running;

	/* Cached properties */
	int  cached_vol; /* 0–100 */
	bool muted;
};

/* -----------------------------------------------------------------------
 * SPI register access
 * ----------------------------------------------------------------------- */

static int cs47l63_reg_write(const struct cs47l63_config *cfg,
			     uint32_t reg, uint32_t value)
{
	uint8_t buf[8];
	uint32_t addr_word = reg << 1U; /* bit 0 = 0 → write */

	buf[0] = (uint8_t)(addr_word >> 24U);
	buf[1] = (uint8_t)(addr_word >> 16U);
	buf[2] = (uint8_t)(addr_word >> 8U);
	buf[3] = (uint8_t)(addr_word);
	buf[4] = (uint8_t)(value >> 24U);
	buf[5] = (uint8_t)(value >> 16U);
	buf[6] = (uint8_t)(value >> 8U);
	buf[7] = (uint8_t)(value);

	const struct spi_buf tx = {.buf = buf, .len = sizeof(buf)};
	const struct spi_buf_set tx_set = {.buffers = &tx, .count = 1U};

	return spi_write_dt(&cfg->spi, &tx_set);
}

static int cs47l63_reg_read(const struct cs47l63_config *cfg,
			    uint32_t reg, uint32_t *value)
{
	uint8_t tx_buf[8] = {0U};
	uint8_t rx_buf[8] = {0U};
	uint32_t addr_word = (reg << 1U) | 0x01U; /* bit 0 = 1 → read */
	int ret;

	tx_buf[0] = (uint8_t)(addr_word >> 24U);
	tx_buf[1] = (uint8_t)(addr_word >> 16U);
	tx_buf[2] = (uint8_t)(addr_word >> 8U);
	tx_buf[3] = (uint8_t)(addr_word);

	const struct spi_buf tx = {.buf = tx_buf, .len = sizeof(tx_buf)};
	const struct spi_buf rx = {.buf = rx_buf, .len = sizeof(rx_buf)};
	const struct spi_buf_set tx_set = {.buffers = &tx, .count = 1U};
	const struct spi_buf_set rx_set = {.buffers = &rx, .count = 1U};

	ret = spi_transceive_dt(&cfg->spi, &tx_set, &rx_set);
	if (ret != 0) {
		return ret;
	}

	*value = ((uint32_t)rx_buf[4] << 24U) |
		 ((uint32_t)rx_buf[5] << 16U) |
		 ((uint32_t)rx_buf[6] << 8U)  |
		 ((uint32_t)rx_buf[7]);
	return 0;
}

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

static int cs47l63_sample_rate_code(uint32_t rate, uint32_t *code)
{
	switch (rate) {
	case 8000U:
		*code = CS47L63_SR_8K;
		break;
	case 16000U:
		*code = CS47L63_SR_16K;
		break;
	case 32000U:
		*code = CS47L63_SR_32K;
		break;
	case 44100U:
		*code = CS47L63_SR_44K1;
		break;
	case 48000U:
		*code = CS47L63_SR_48K;
		break;
	case 96000U:
		*code = CS47L63_SR_96K;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

/*
 * Map vol (0–100) to DAC volume register word.
 * vol=0  → −32 dB (field=0x80), vol=100 → 0 dB (field=0xBF).
 */
static uint32_t cs47l63_vol_word(int vol, bool muted)
{
	uint32_t field;

	if (muted) {
		return CS47L63_DAC_MUTE;
	}

	vol = CLAMP(vol, 0, 100);
	field = CS47L63_DAC_VOL_MINDB +
		(uint32_t)(vol * (CS47L63_DAC_VOL_0DB - CS47L63_DAC_VOL_MINDB) / 100U);

	return field << CS47L63_DAC_VOL_SHIFT;
}

static int cs47l63_apply_volume(const struct device *dev)
{
	const struct cs47l63_config *cfg = dev->config;
	const struct cs47l63_data *data = dev->data;
	uint32_t word = cs47l63_vol_word(data->cached_vol, data->muted);
	int ret;

	ret = cs47l63_reg_write(cfg, CS47L63_DAC_DIGITAL_VOLUME_1L, word);
	if (ret != 0) {
		return ret;
	}
	return cs47l63_reg_write(cfg, CS47L63_DAC_DIGITAL_VOLUME_1R, word);
}

/* -----------------------------------------------------------------------
 * TX streaming thread
 * ----------------------------------------------------------------------- */

/*
 * Duplicate each 16-bit mono sample into left and right I2S channels.
 */
static void mono16_to_stereo16(const int16_t *src, int16_t *dst, size_t n)
{
	for (size_t i = 0U; i < n; i++) {
		dst[2U * i]      = src[i];
		dst[2U * i + 1U] = src[i];
	}
}

/*
 * TX thread function.
 *
 * Keeps the I2S stream running by:
 *   1. Pre-queuing two silence blocks to prime the pipeline.
 *   2. Repeatedly allocating a DMA block, signalling the application
 *      (via tx_done_cb) to fill it, then submitting it to the I2S driver.
 *
 * The k_mem_slab_alloc() call in the loop blocks until the I2S driver
 * frees a completed block back to the slab, which rate-limits the loop
 * to the hardware playback speed without any additional sleeps.
 */
static void cs47l63_tx_thread_fn(void *arg1, void *arg2, void *arg3)
{
	const struct device *dev = (const struct device *)arg1;
	const struct cs47l63_config *cfg = dev->config;
	struct cs47l63_data *data = dev->data;
	void *block;
	int i;
	int ret;

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	/* Prime the I2S pipeline with two silence blocks. */
	for (i = 0; i < 2; i++) {
		ret = k_mem_slab_alloc(&data->tx_slab, &block, K_FOREVER);
		if (ret < 0) {
			LOG_ERR("Failed to alloc I2S prime block: %d", ret);
			return;
		}
		(void)memset(block, 0, CS47L63_I2S_BLOCK_SIZE);
		ret = i2s_write(cfg->i2s_dev, block, CS47L63_I2S_BLOCK_SIZE);
		if (ret < 0) {
			LOG_ERR("i2s_write (prime) failed: %d", ret);
			k_mem_slab_free(&data->tx_slab, block);
			return;
		}
	}

	ret = i2s_trigger(cfg->i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		LOG_ERR("I2S TRIGGER_START failed: %d", ret);
		return;
	}

	while (data->running) {
		/*
		 * Allocate the next DMA block.  Blocks until the I2S driver
		 * returns a completed buffer to the slab — this is the natural
		 * flow-control mechanism.
		 */
		ret = k_mem_slab_alloc(&data->tx_slab, &block, K_MSEC(500));
		if (ret < 0) {
			/* Timeout — check running flag and retry. */
			continue;
		}

		data->current_block = block;

		/* Invoke the application callback to fill the block. */
		if (data->tx_done_cb != NULL) {
			data->tx_done_cb(dev, data->tx_done_user_data);
		}

		/*
		 * If the application did not call write() (e.g. ring-buffer
		 * underflow), fill with silence to prevent I2S underrun.
		 */
		if (data->current_block != NULL) {
			(void)memset(data->current_block, 0,
				     CS47L63_I2S_BLOCK_SIZE);
			ret = i2s_write(cfg->i2s_dev, data->current_block,
					CS47L63_I2S_BLOCK_SIZE);
			if (ret < 0) {
				LOG_WRN("i2s_write (silence) failed: %d", ret);
				k_mem_slab_free(&data->tx_slab,
						data->current_block);
			}
			data->current_block = NULL;
		}
	}

	(void)i2s_trigger(cfg->i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
}

/* -----------------------------------------------------------------------
 * audio_codec API implementation
 * ----------------------------------------------------------------------- */

static int cs47l63_configure(const struct device *dev,
			     struct audio_codec_cfg *codec_cfg)
{
	const struct cs47l63_config *cfg = dev->config;
	struct cs47l63_data *data = dev->data;
	uint32_t sr_code;
	uint8_t wl_code;
	uint32_t sysclk_val;
	struct i2s_config i2s_cfg;
	int ret;

	if (codec_cfg->dai_route != AUDIO_ROUTE_PLAYBACK &&
	    codec_cfg->dai_route != AUDIO_ROUTE_BYPASS) {
		return -ENOTSUP;
	}

	if (codec_cfg->dai_type != AUDIO_DAI_TYPE_PCM) {
		LOG_ERR("Only PCM DAI type supported (got %d)",
			codec_cfg->dai_type);
		return -ENOTSUP;
	}

	const struct pcm_config *pcm = &codec_cfg->dai_cfg.pcm;

	ret = cs47l63_sample_rate_code((uint32_t)pcm->samplerate, &sr_code);
	if (ret != 0) {
		LOG_ERR("Unsupported sample rate %u", (unsigned)pcm->samplerate);
		return ret;
	}

	switch (pcm->pcm_width) {
	case AUDIO_PCM_WIDTH_16_BITS:
		wl_code = CS47L63_AIF_WL_16;
		break;
	case AUDIO_PCM_WIDTH_24_BITS:
		wl_code = CS47L63_AIF_WL_24;
		break;
	case AUDIO_PCM_WIDTH_32_BITS:
		wl_code = CS47L63_AIF_WL_32;
		break;
	default:
		return -ENOTSUP;
	}

	data->sample_rate = (uint32_t)pcm->samplerate;
	data->channels    = pcm->channels;
	data->word_size   = (uint8_t)pcm->pcm_width;
	data->block_size  = pcm->block_size;

	/*
	 * SYSTEM_CLOCK_1: enable SYSCLK from MCLK1 at 12.288 MHz.
	 *
	 * The nRF5340 I2S peripheral generates a 12.288 MHz MCK that is
	 * routed to the CS47L63 MCLK1 input on the nRF5340 Audio DK PCB.
	 */
	sysclk_val = ((uint32_t)CS47L63_SYSCLK_FREQ_12MHZ <<
			CS47L63_SYSCLK_FREQ_SHIFT) |
		     CS47L63_SYSCLK_ENA |
		     CS47L63_SYSCLK_SRC_MCLK1;

	ret = cs47l63_reg_write(cfg, CS47L63_SYSTEM_CLOCK_1, sysclk_val);
	if (ret != 0) {
		LOG_ERR("SYSTEM_CLOCK_1 write failed: %d", ret);
		return ret;
	}

	/* SAMPLE_RATE_1: internal DSP sample rate. */
	ret = cs47l63_reg_write(cfg, CS47L63_SAMPLE_RATE_1, sr_code);
	if (ret != 0) {
		return ret;
	}

	/* OUTPUT_RATE_1: output path sample rate. */
	ret = cs47l63_reg_write(cfg, CS47L63_OUTPUT_RATE_1, sr_code);
	if (ret != 0) {
		return ret;
	}

	/* AIF1 as I2S slave (nRF5340 is master). */
	ret = cs47l63_reg_write(cfg, CS47L63_AIF1_BCLK_CTRL,
				CS47L63_AIF_BCLK_SLAVE);
	if (ret != 0) {
		return ret;
	}

	/* I2S framing format. */
	ret = cs47l63_reg_write(cfg, CS47L63_AIF1_FORMAT, CS47L63_AIF_FMT_I2S);
	if (ret != 0) {
		return ret;
	}

	/* Word length (bits per sample). */
	ret = cs47l63_reg_write(cfg, CS47L63_AIF1_FRAME_CTRL_1, wl_code);
	if (ret != 0) {
		return ret;
	}

	/*
	 * AIF1_FRAME_CTRL_2: reset to default (0).
	 * Default = 2 slots per frame, which is correct for stereo I2S.
	 */
	ret = cs47l63_reg_write(cfg, CS47L63_AIF1_FRAME_CTRL_2, 0U);
	if (ret != 0) {
		return ret;
	}

	/* Enable AIF1 receive path. */
	ret = cs47l63_reg_write(cfg, CS47L63_AIF1_ENABLES_1, CS47L63_AIF_RX_ENA);
	if (ret != 0) {
		return ret;
	}

	/*
	 * Output routing:
	 *   OUT1L ← AIF1 RX1 (left channel at 0 dB)
	 *   OUT1R ← AIF1 RX2 (right channel at 0 dB)
	 */
	ret = cs47l63_reg_write(cfg, CS47L63_OUT1LMIX_INPUT_1_SOURCE,
				CS47L63_SRC_AIF1RX1);
	if (ret != 0) {
		return ret;
	}

	ret = cs47l63_reg_write(cfg, CS47L63_OUT1LMIX_INPUT_1_VOLUME,
				CS47L63_MIX_VOL_0DB);
	if (ret != 0) {
		return ret;
	}

	ret = cs47l63_reg_write(cfg, CS47L63_OUT1RMIX_INPUT_1_SOURCE,
				CS47L63_SRC_AIF1RX2);
	if (ret != 0) {
		return ret;
	}

	ret = cs47l63_reg_write(cfg, CS47L63_OUT1RMIX_INPUT_1_VOLUME,
				CS47L63_MIX_VOL_0DB);
	if (ret != 0) {
		return ret;
	}

	/* Enable OUT1L and OUT1R. */
	ret = cs47l63_reg_write(cfg, CS47L63_OUTPUT_ENABLES_1,
				CS47L63_OUT1L_ENA | CS47L63_OUT1R_ENA);
	if (ret != 0) {
		return ret;
	}

	/* Apply the cached volume settings. */
	ret = cs47l63_apply_volume(dev);
	if (ret != 0) {
		return ret;
	}

	/*
	 * Configure the nRF5340 I2S peripheral.
	 * The I2S bus is always stereo even when the codec stream is mono;
	 * the driver handles mono-to-stereo conversion in write().
	 */
	i2s_cfg.word_size      = pcm->pcm_width;
	i2s_cfg.channels       = 2U;
	/*
	 * I2S format: both the nRF5340 I2S peripheral and the CS47L63 AIF1
	 * are configured for standard I2S framing.  The codec register value
	 * CS47L63_AIF_FMT_I2S (line ~557) must remain in sync with this.
	 */
	i2s_cfg.format         = I2S_FMT_DATA_FORMAT_I2S;
	i2s_cfg.options        = I2S_OPT_FRAME_CLK_CONTROLLER |
				  I2S_OPT_BIT_CLK_CONTROLLER;
	i2s_cfg.frame_clk_freq = (uint32_t)pcm->samplerate;
	i2s_cfg.mem_slab       = &data->tx_slab;
	i2s_cfg.block_size     = CS47L63_I2S_BLOCK_SIZE;
	i2s_cfg.timeout        = CS47L63_I2S_TIMEOUT_MS;

	ret = i2s_configure(cfg->i2s_dev, I2S_DIR_TX, &i2s_cfg);
	if (ret != 0) {
		LOG_ERR("i2s_configure failed: %d", ret);
		return ret;
	}

	LOG_INF("CS47L63: %u Hz, %u ch, %u-bit, block=%zu B",
		(unsigned)pcm->samplerate, pcm->channels,
		(unsigned)pcm->pcm_width, pcm->block_size);

	return 0;
}

static void cs47l63_start_output(const struct device *dev)
{
	/* Intentionally empty: output is activated in start(). */
	ARG_UNUSED(dev);
}

static void cs47l63_stop_output(const struct device *dev)
{
	/* Intentionally empty: output is deactivated in stop(). */
	ARG_UNUSED(dev);
}

static int cs47l63_start(const struct device *dev, audio_dai_dir_t dir)
{
	const struct cs47l63_config *cfg = dev->config;
	struct cs47l63_data *data = dev->data;

	if ((dir & AUDIO_DAI_DIR_TX) == 0U) {
		return -ENOTSUP;
	}

	if (data->running) {
		return 0;
	}

	data->running = true;

	k_thread_create(&data->tx_thread, cfg->tx_stack, CS47L63_TX_STACK_SIZE,
			cs47l63_tx_thread_fn, (void *)dev, NULL, NULL,
			CONFIG_AUDIO_CODEC_CS47L63_TX_THREAD_PRIORITY, 0U, K_NO_WAIT);
	k_thread_name_set(&data->tx_thread, "cs47l63_tx");

	return 0;
}

static int cs47l63_stop(const struct device *dev, audio_dai_dir_t dir)
{
	const struct cs47l63_config *cfg = dev->config;
	struct cs47l63_data *data = dev->data;

	if ((dir & AUDIO_DAI_DIR_TX) == 0U) {
		return -ENOTSUP;
	}

	if (!data->running) {
		return 0;
	}

	data->running = false;

	/* Abort the TX thread and wait for it to terminate. */
	k_thread_abort(&data->tx_thread);
	(void)k_thread_join(&data->tx_thread, K_MSEC(1000));
	(void)i2s_trigger(cfg->i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);

	return 0;
}

static int cs47l63_write(const struct device *dev, uint8_t *audio_data,
			 size_t data_size)
{
	const struct cs47l63_config *cfg = dev->config;
	struct cs47l63_data *data = dev->data;
	int ret;

	if (data->current_block == NULL) {
		return -EBUSY;
	}

	if (data->channels == 1U) {
		/*
		 * Mono input: duplicate each sample into left and right
		 * channels.  The I2S block is always stereo 16-bit.
		 */
		size_t n = data_size / sizeof(int16_t);

		mono16_to_stereo16((const int16_t *)audio_data,
				   (int16_t *)data->current_block, n);
	} else {
		(void)memcpy(data->current_block, audio_data,
			     MIN(data_size, CS47L63_I2S_BLOCK_SIZE));
	}

	ret = i2s_write(cfg->i2s_dev, data->current_block,
			CS47L63_I2S_BLOCK_SIZE);
	data->current_block = NULL;

	if (ret < 0) {
		LOG_WRN("i2s_write failed: %d", ret);
		return ret;
	}

	return (int)data_size;
}

static int cs47l63_set_property(const struct device *dev,
				audio_property_t property,
				audio_channel_t channel,
				audio_property_value_t val)
{
	struct cs47l63_data *data = dev->data;

	switch (property) {
	case AUDIO_PROPERTY_OUTPUT_VOLUME:
		data->cached_vol = val.vol;
		break;
	case AUDIO_PROPERTY_OUTPUT_MUTE:
		data->muted = val.mute;
		break;
	default:
		return -ENOTSUP;
	}

	ARG_UNUSED(channel);
	return 0;
}

static int cs47l63_apply_properties(const struct device *dev)
{
	return cs47l63_apply_volume(dev);
}

static int cs47l63_register_done_callback(
	const struct device *dev,
	audio_codec_tx_done_callback_t tx_cb, void *tx_user,
	audio_codec_rx_done_callback_t rx_cb, void *rx_user)
{
	struct cs47l63_data *data = dev->data;

	ARG_UNUSED(rx_cb);
	ARG_UNUSED(rx_user);

	data->tx_done_cb        = tx_cb;
	data->tx_done_user_data = tx_user;

	return 0;
}

static const struct audio_codec_api cs47l63_driver_api = {
	.configure             = cs47l63_configure,
	.start_output          = cs47l63_start_output,
	.stop_output           = cs47l63_stop_output,
	.set_property          = cs47l63_set_property,
	.apply_properties      = cs47l63_apply_properties,
	.start                 = cs47l63_start,
	.stop                  = cs47l63_stop,
	.write                 = cs47l63_write,
	.register_done_callback = cs47l63_register_done_callback,
};

/* -----------------------------------------------------------------------
 * Driver initialisation
 * ----------------------------------------------------------------------- */

static int cs47l63_init(const struct device *dev)
{
	const struct cs47l63_config *cfg = dev->config;
	struct cs47l63_data *data = dev->data;
	uint32_t devid;
	uint32_t revid;
	int ret;

	if (!spi_is_ready_dt(&cfg->spi)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}

	if (!device_is_ready(cfg->i2s_dev)) {
		LOG_ERR("I2S device not ready");
		return -ENODEV;
	}

	ret = k_mem_slab_init(&data->tx_slab, data->tx_slab_buf,
			      CS47L63_I2S_BLOCK_SIZE, CS47L63_TX_SLAB_BLOCKS);
	if (ret < 0) {
		LOG_ERR("k_mem_slab_init failed: %d", ret);
		return ret;
	}

	/* Hardware reset: assert (active-low), wait, then deassert. */
	if (cfg->reset_gpio.port != NULL) {
		ret = gpio_pin_configure_dt(&cfg->reset_gpio,
					    GPIO_OUTPUT_ACTIVE);
		if (ret != 0) {
			LOG_ERR("reset GPIO configure failed: %d", ret);
			return ret;
		}
		k_sleep(K_MSEC(2));
		ret = gpio_pin_set_dt(&cfg->reset_gpio, 0);
		if (ret != 0) {
			return ret;
		}
		/* Chip requires at least 1 ms after reset de-assertion. */
		k_sleep(K_MSEC(5));
	}

	/* Verify device ID. */
	ret = cs47l63_reg_read(cfg, CS47L63_DEVID, &devid);
	if (ret != 0) {
		LOG_ERR("DEVID read failed: %d", ret);
		return ret;
	}

	if ((devid & 0xFFFFU) != CS47L63_DEVID_VAL) {
		LOG_ERR("Unexpected DEVID 0x%04X (expected 0x%04X)",
			devid & 0xFFFFU, CS47L63_DEVID_VAL);
		return -ENODEV;
	}

	ret = cs47l63_reg_read(cfg, CS47L63_REVID, &revid);
	if (ret == 0) {
		LOG_INF("CS47L63 revision 0x%02X", revid & 0xFFU);
	}

	data->cached_vol = 50;
	data->muted      = false;
	data->running    = false;

	return 0;
}

/* -----------------------------------------------------------------------
 * Device instantiation
 * ----------------------------------------------------------------------- */

#define CS47L63_INIT(inst)                                                  \
	K_THREAD_STACK_DEFINE(cs47l63_tx_stack_##inst, CS47L63_TX_STACK_SIZE); \
									    \
	static struct cs47l63_data cs47l63_data_##inst;                     \
									    \
	static const struct cs47l63_config cs47l63_config_##inst = {        \
		.spi = SPI_DT_SPEC_INST_GET(                                \
			inst,                                               \
			SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB |             \
			SPI_WORD_SET(8U),                                   \
			0U),                                                \
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios,   \
						       {0}),                \
		.i2s_dev    = DEVICE_DT_GET(DT_INST_PHANDLE(inst, i2s_bus)), \
		.tx_stack   = cs47l63_tx_stack_##inst,                      \
	};                                                                  \
									    \
	DEVICE_DT_INST_DEFINE(inst, cs47l63_init, NULL,                     \
			      &cs47l63_data_##inst,                         \
			      &cs47l63_config_##inst,                       \
			      POST_KERNEL,                                  \
			      CONFIG_AUDIO_CODEC_INIT_PRIORITY,             \
			      &cs47l63_driver_api)

DT_INST_FOREACH_STATUS_OKAY(CS47L63_INIT)
