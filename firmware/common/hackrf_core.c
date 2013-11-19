/*
 * Copyright 2012 Michael Ossmann <mike@ossmann.com>
 * Copyright 2012 Jared Boone <jared@sharebrained.com>
 * Copyright 2013 Benjamin Vernoux <titanmkd@gmail.com>
 *
 * This file is part of HackRF.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "hackrf_core.h"
#include "si5351c.h"
#include "max2837.h"
#include "rffc5071.h"
#include <libopencm3/lpc43xx/i2c.h>
#include <libopencm3/lpc43xx/cgu.h>
#include <libopencm3/lpc43xx/gpio.h>
#include <libopencm3/lpc43xx/scu.h>
#include <libopencm3/lpc43xx/ssp.h>

void delay(uint32_t duration)
{
	uint32_t i;

	for (i = 0; i < duration; i++)
		__asm__("nop");
}

/* GCD algo from wikipedia */
/* http://en.wikipedia.org/wiki/Greatest_common_divisor */
static uint32_t
gcd(uint32_t u, uint32_t v)
{
	int s;

	if (!u || !v)
		return u | v;

	for (s=0; !((u|v)&1); s++) {
		u >>= 1;
		v >>= 1;
	}

	while (!(u&1))
		u >>= 1;

	do {
		while (!(v&1))
			v >>= 1;

		if (u>v) {
			uint32_t t;
			t = v;
			v = u;
			u = t;
		}

		v = v - u;
	}
	while (v);

	return u << s;
}

bool sample_rate_frac_set(uint32_t rate_num, uint32_t rate_denom)
{
	const uint64_t VCO_FREQ = 800 * 1000 * 1000; /* 800 MHz */
	uint32_t MSx_P1,MSx_P2,MSx_P3;
	uint32_t a, b, c;
	uint32_t rem;

	/* Find best config */
	a = (VCO_FREQ * rate_denom) / rate_num;

	rem = (VCO_FREQ * rate_denom) - (a * rate_num);

	if (!rem) {
		/* Integer mode */
		b = 0;
		c = 1;
	} else {
		/* Fractional */
		uint32_t g = gcd(rem, rate_num);
		rem /= g;
		rate_num /= g;

		if (rate_num < (1<<20)) {
			/* Perfect match */
			b = rem;
			c = rate_num;
		} else {
			/* Approximate */
			c = (1<<20) - 1;
			b = ((uint64_t)c * (uint64_t)rem) / rate_num;

			g = gcd(b, c);
			b /= g;
			c /= g;
		}
	}

	/* Can we enable integer mode ? */
	if (a & 0x1 || b)
		si5351c_set_int_mode(0, 0);
	else
		si5351c_set_int_mode(0, 1);

	/* Final MS values */
	MSx_P1 = 128*a + (128 * b/c) - 512;
	MSx_P2 = (128*b) % c;
	MSx_P3 = c;

	/* MS0/CLK0 is the source for the MAX5864/CPLD (CODEC_CLK). */
	si5351c_configure_multisynth(0, MSx_P1, MSx_P2, MSx_P3, 1);

	/* MS0/CLK1 is the source for the CPLD (CODEC_X2_CLK). */
	si5351c_configure_multisynth(1, 0, 0, 0, 0);//p1 doesn't matter

	/* MS0/CLK2 is the source for SGPIO (CODEC_X2_CLK) */
	si5351c_configure_multisynth(2, 0, 0, 0, 0);//p1 doesn't matter

	/* MS0/CLK3 is the source for the external clock output. */
	//si5351c_configure_multisynth(3, p1, 0, 1, 0); // no clk out

	return true;
}

bool sample_rate_set(const uint32_t sample_rate_hz) {
#ifdef JELLYBEAN
	/* Due to design issues, Jellybean/Lemondrop frequency plan is limited.
	 * Long version of the story: The MAX2837 reference frequency
	 * originates from the same PLL as the sample clocks, and in order to
	 * keep the sample clocks in phase and keep jitter noise down, the MAX2837
	 * and sample clocks must be integer-related.
	 */
	uint32_t r_div_sample = 2;
	uint32_t r_div_sgpio = 1;
	
	switch( sample_rate_hz ) {
	case 5000000:
		r_div_sample = 3;	/* 800 MHz / 20 / 8 =  5 MHz */
		r_div_sgpio = 2;	/* 800 MHz / 20 / 4 = 10 MHz */
		break;
		
	case 10000000:
		r_div_sample = 2;	/* 800 MHz / 20 / 4 = 10 MHz */
		r_div_sgpio = 1;	/* 800 MHz / 20 / 2 = 20 MHz */
		break;
		
	case 20000000:
		r_div_sample = 1;	/* 800 MHz / 20 / 2 = 20 MHz */
		r_div_sgpio = 0;	/* 800 MHz / 20 / 1 = 40 MHz */
		break;
		
	default:
		return false;
	}
	
	/* NOTE: Because MS1, 2, 3 outputs are slaved to PLLA, the p1, p2, p3
	 * values are irrelevant. */
	
	/* MS0/CLK1 is the source for the MAX5864 codec. */
	si5351c_configure_multisynth(1, 4608, 0, 1, r_div_sample);

	/* MS0/CLK2 is the source for the CPLD codec clock (same as CLK1). */
	si5351c_configure_multisynth(2, 4608, 0, 1, r_div_sample);

	/* MS0/CLK3 is the source for the SGPIO clock. */
	si5351c_configure_multisynth(3, 4608, 0, 1, r_div_sgpio);
	
	return true;
#endif

#if (defined JAWBREAKER || defined HACKRF_ONE)
	uint32_t p1 = 4608;
	
 	switch(sample_rate_hz) {
	case 8000000:
		p1 = SI_INTDIV(50);	// 800MHz / 50 = 16 MHz (SGPIO), 8 MHz (codec)
		break;
		
 	case 10000000:
		p1 = SI_INTDIV(40);	// 800MHz / 40 = 20 MHz (SGPIO), 10 MHz (codec)
 		break;
 
 	case 12500000:
		p1 = SI_INTDIV(32);	// 800MHz / 32 = 25 MHz (SGPIO), 12.5 MHz (codec)
 		break;
 
 	case 16000000:
		p1 = SI_INTDIV(25);	// 800MHz / 25 = 32 MHz (SGPIO), 16 MHz (codec)
 		break;
 	
 	case 20000000:
		p1 = SI_INTDIV(20);	// 800MHz / 20 = 40 MHz (SGPIO), 20 MHz (codec)
 		break;
	
	default:
		return false;
	}
	
	/* MS0/CLK0 is the source for the MAX5864/CPLD (CODEC_CLK). */
	si5351c_configure_multisynth(0, p1, 0, 1, 1);

	/* MS0/CLK1 is the source for the CPLD (CODEC_X2_CLK). */
	si5351c_configure_multisynth(1, p1, 0, 1, 0);//p1 doesn't matter

	/* MS0/CLK2 is the source for SGPIO (CODEC_X2_CLK) */
	si5351c_configure_multisynth(2, p1, 0, 1, 0);//p1 doesn't matter

	/* MS0/CLK3 is the source for the external clock output. */
	//si5351c_configure_multisynth(3, p1, 0, 1, 0); // no clk out

	return true;
#endif
}

bool baseband_filter_bandwidth_set(const uint32_t bandwidth_hz) {
	return max2837_set_lpf_bandwidth(bandwidth_hz);
}

/* clock startup for Jellybean with Lemondrop attached */ 
void cpu_clock_init(void)
{
	/* use IRC as clock source for APB1 (including I2C0) */
	CGU_BASE_APB1_CLK = CGU_BASE_APB1_CLK_CLK_SEL(CGU_SRC_IRC);

	i2c0_init(15);

	si5351c_disable_all_outputs();
	si5351c_disable_oeb_pin_control();
	si5351c_power_down_all_clocks();
	si5351c_set_crystal_configuration();
	si5351c_enable_xo_and_ms_fanout();
	si5351c_configure_pll_sources_for_xtal();
	si5351c_configure_pll1_multisynth();

#ifdef JELLYBEAN
	/*
	 * Jellybean/Lemondrop clocks:
	 *   CLK0 -> MAX2837
	 *   CLK1 -> MAX5864/CPLD.GCLK0
	 *   CLK2 -> CPLD.GCLK1
	 *   CLK3 -> CPLD.GCLK2
	 *   CLK4 -> LPC4330
	 *   CLK5 -> RFFC5072
	 *   CLK6 -> extra
	 *   CLK7 -> extra
	 */

	/* MS0/CLK0 is the source for the MAX2837 clock input. */
	si5351c_configure_multisynth(0, 2048, 0, 1, 0); /* 40MHz */

	/* MS4/CLK4 is the source for the LPC43xx microcontroller. */
	si5351c_configure_multisynth(4, 8021, 0, 3, 0); /* 12MHz */

	/* MS5/CLK5 is the source for the RFFC5071 mixer. */
	si5351c_configure_multisynth(5, 1536, 0, 1, 0); /* 50MHz */
#endif

#if (defined JAWBREAKER || defined HACKRF_ONE)
	/*
	 * Jawbreaker clocks:
	 *   CLK0 -> MAX5864/CPLD
	 *   CLK1 -> CPLD
	 *   CLK2 -> SGPIO
	 *   CLK3 -> external clock output
	 *   CLK4 -> RFFC5072
	 *   CLK5 -> MAX2837
	 *   CLK6 -> none
	 *   CLK7 -> LPC4330 (but LPC4330 starts up on its own crystal)
	 */

	/* MS4/CLK4 is the source for the RFFC5071 mixer. */
	si5351c_configure_multisynth(4, 16*128-512, 0, 1, 0); /* 800/16 = 50MHz */
 
 	/* MS5/CLK5 is the source for the MAX2837 clock input. */
	si5351c_configure_multisynth(5, 20*128-512, 0, 1, 0); /* 800/20 = 40MHz */

	/* MS7/CLK7 is the source for the LPC43xx microcontroller. */
	//uint8_t ms7data[] = { 91, 40, 0x0 };
	//si5351c_write(ms7data, sizeof(ms7data));
#endif

	/* Set to 10 MHz, the common rate between Jellybean and Jawbreaker. */
	sample_rate_set(10000000);

	si5351c_configure_clock_control();
	// soft reset
	uint8_t resetdata[] = { 177, 0xac };
	si5351c_write(resetdata, sizeof(resetdata));
	si5351c_enable_clock_outputs();

	//FIXME disable I2C
	/* Kick I2C0 down to 400kHz when we switch over to APB1 clock = 204MHz */
	i2c0_init(255);

	/*
	 * 12MHz clock is entering LPC XTAL1/OSC input now.  On
	 * Jellybean/Lemondrop, this is a signal from the clock generator.  On
	 * Jawbreaker, there is a 12 MHz crystal at the LPC.
	 * Set up PLL1 to run from XTAL1 input.
	 */

	//FIXME a lot of the details here should be in a CGU driver

#ifdef JELLYBEAN
	/* configure xtal oscillator for external clock input signal */
	CGU_XTAL_OSC_CTRL |= CGU_XTAL_OSC_CTRL_BYPASS;
#endif

	/* set xtal oscillator to low frequency mode */
	CGU_XTAL_OSC_CTRL &= ~CGU_XTAL_OSC_CTRL_HF;

	/* power on the oscillator and wait until stable */
	CGU_XTAL_OSC_CTRL &= ~CGU_XTAL_OSC_CTRL_ENABLE;

	/* use XTAL_OSC as clock source for BASE_M4_CLK (CPU) */
	CGU_BASE_M4_CLK = CGU_BASE_M4_CLK_CLK_SEL(CGU_SRC_XTAL);

	/* use XTAL_OSC as clock source for APB1 */
	CGU_BASE_APB1_CLK = CGU_BASE_APB1_CLK_AUTOBLOCK
			| CGU_BASE_APB1_CLK_CLK_SEL(CGU_SRC_XTAL);

	/* use XTAL_OSC as clock source for PLL1 */
	/* Start PLL1 at 12MHz * 17 / (2+2) = 51MHz. */
	CGU_PLL1_CTRL = CGU_PLL1_CTRL_CLK_SEL(CGU_SRC_XTAL)
            | CGU_PLL1_CTRL_PSEL(1)
            | CGU_PLL1_CTRL_NSEL(0)
            | CGU_PLL1_CTRL_MSEL(16)
            | CGU_PLL1_CTRL_PD;

	/* power on PLL1 and wait until stable */
	CGU_PLL1_CTRL &= ~CGU_PLL1_CTRL_PD;
	while (!(CGU_PLL1_STAT & CGU_PLL1_STAT_LOCK));

	/* use PLL1 as clock source for BASE_M4_CLK (CPU) */
	CGU_BASE_M4_CLK = CGU_BASE_M4_CLK_CLK_SEL(CGU_SRC_PLL1);

	/* Move PLL1 up to 12MHz * 17 = 204MHz. */
	CGU_PLL1_CTRL = CGU_PLL1_CTRL_CLK_SEL(CGU_SRC_XTAL)
            | CGU_PLL1_CTRL_PSEL(0)
            | CGU_PLL1_CTRL_NSEL(0)
			| CGU_PLL1_CTRL_MSEL(16)
			| CGU_PLL1_CTRL_FBSEL;
			//| CGU_PLL1_CTRL_DIRECT;

	/* wait until stable */
	while (!(CGU_PLL1_STAT & CGU_PLL1_STAT_LOCK));

	/* use XTAL_OSC as clock source for PLL0USB */
	CGU_PLL0USB_CTRL = CGU_PLL0USB_CTRL_PD
			| CGU_PLL0USB_CTRL_AUTOBLOCK
			| CGU_PLL0USB_CTRL_CLK_SEL(CGU_SRC_XTAL);
	while (CGU_PLL0USB_STAT & CGU_PLL0USB_STAT_LOCK);

	/* configure PLL0USB to produce 480 MHz clock from 12 MHz XTAL_OSC */
	/* Values from User Manual v1.4 Table 94, for 12MHz oscillator. */
	CGU_PLL0USB_MDIV = 0x06167FFA;
	CGU_PLL0USB_NP_DIV = 0x00302062;
	CGU_PLL0USB_CTRL |= (CGU_PLL0USB_CTRL_PD
			| CGU_PLL0USB_CTRL_DIRECTI
			| CGU_PLL0USB_CTRL_DIRECTO
			| CGU_PLL0USB_CTRL_CLKEN);

	/* power on PLL0USB and wait until stable */
	CGU_PLL0USB_CTRL &= ~CGU_PLL0USB_CTRL_PD;
	while (!(CGU_PLL0USB_STAT & CGU_PLL0USB_STAT_LOCK));

	/* use PLL0USB as clock source for USB0 */
	CGU_BASE_USB0_CLK = CGU_BASE_USB0_CLK_AUTOBLOCK
			| CGU_BASE_USB0_CLK_CLK_SEL(CGU_SRC_PLL0USB);

	/* Switch peripheral clock over to use PLL1 (204MHz) */
	CGU_BASE_PERIPH_CLK = CGU_BASE_PERIPH_CLK_AUTOBLOCK
			| CGU_BASE_PERIPH_CLK_CLK_SEL(CGU_SRC_PLL1);

	/* Switch APB1 clock over to use PLL1 (204MHz) */
	CGU_BASE_APB1_CLK = CGU_BASE_APB1_CLK_AUTOBLOCK
			| CGU_BASE_APB1_CLK_CLK_SEL(CGU_SRC_PLL1);
}

void ssp1_init(void)
{
	/*
	 * Configure CS_AD pin to keep the MAX5864 SPI disabled while we use the
	 * SPI bus for the MAX2837. FIXME: this should probably be somewhere else.
	 */
	scu_pinmux(SCU_AD_CS, SCU_GPIO_FAST);
	GPIO_SET(PORT_AD_CS) = PIN_AD_CS;
	GPIO_DIR(PORT_AD_CS) |= PIN_AD_CS;

	scu_pinmux(SCU_XCVR_CS, SCU_GPIO_FAST);
	GPIO_SET(PORT_XCVR_CS) = PIN_XCVR_CS;
	GPIO_DIR(PORT_XCVR_CS) |= PIN_XCVR_CS;
	
	/* Configure SSP1 Peripheral (to be moved later in SSP driver) */
	scu_pinmux(SCU_SSP1_MISO, (SCU_SSP_IO | SCU_CONF_FUNCTION5));
	scu_pinmux(SCU_SSP1_MOSI, (SCU_SSP_IO | SCU_CONF_FUNCTION5));
	scu_pinmux(SCU_SSP1_SCK,  (SCU_SSP_IO | SCU_CONF_FUNCTION1));
}

void ssp1_set_mode_max2837(void)
{
	/* FIXME speed up once everything is working reliably */
	/*
	// Freq About 0.0498MHz / 49.8KHz => Freq = PCLK / (CPSDVSR * [SCR+1]) with PCLK=PLL1=204MHz
	const uint8_t serial_clock_rate = 32;
	const uint8_t clock_prescale_rate = 128;
	*/
	// Freq About 4.857MHz => Freq = PCLK / (CPSDVSR * [SCR+1]) with PCLK=PLL1=204MHz
	const uint8_t serial_clock_rate = 21;
	const uint8_t clock_prescale_rate = 2;
	
	ssp_init(SSP1_NUM,
		SSP_DATA_16BITS,
		SSP_FRAME_SPI,
		SSP_CPOL_0_CPHA_0,
		serial_clock_rate,
		clock_prescale_rate,
		SSP_MODE_NORMAL,
		SSP_MASTER,
		SSP_SLAVE_OUT_ENABLE);
}

void ssp1_set_mode_max5864(void)
{
	/* FIXME speed up once everything is working reliably */
	/*
	// Freq About 0.0498MHz / 49.8KHz => Freq = PCLK / (CPSDVSR * [SCR+1]) with PCLK=PLL1=204MHz
	const uint8_t serial_clock_rate = 32;
	const uint8_t clock_prescale_rate = 128;
	*/
	// Freq About 4.857MHz => Freq = PCLK / (CPSDVSR * [SCR+1]) with PCLK=PLL1=204MHz
	const uint8_t serial_clock_rate = 21;
	const uint8_t clock_prescale_rate = 2;
	
	ssp_init(SSP1_NUM,
		SSP_DATA_8BITS,
		SSP_FRAME_SPI,
		SSP_CPOL_0_CPHA_0,
		serial_clock_rate,
		clock_prescale_rate,
		SSP_MODE_NORMAL,
		SSP_MASTER,
		SSP_SLAVE_OUT_ENABLE);
}

void pin_setup(void) {
	/* Release CPLD JTAG pins */
	scu_pinmux(SCU_PINMUX_CPLD_TDO, SCU_GPIO_NOPULL | SCU_CONF_FUNCTION4);
	scu_pinmux(SCU_PINMUX_CPLD_TCK, SCU_GPIO_NOPULL | SCU_CONF_FUNCTION0);
	scu_pinmux(SCU_PINMUX_CPLD_TMS, SCU_GPIO_NOPULL | SCU_CONF_FUNCTION0);
	scu_pinmux(SCU_PINMUX_CPLD_TDI, SCU_GPIO_NOPULL | SCU_CONF_FUNCTION0);
	
	GPIO_DIR(PORT_CPLD_TDO) &= ~PIN_CPLD_TDO;
	GPIO_DIR(PORT_CPLD_TCK) &= ~PIN_CPLD_TCK;
	GPIO_DIR(PORT_CPLD_TMS) &= ~PIN_CPLD_TMS;
	GPIO_DIR(PORT_CPLD_TDI) &= ~PIN_CPLD_TDI;
	
	/* Configure SCU Pin Mux as GPIO */
	scu_pinmux(SCU_PINMUX_LED1, SCU_GPIO_NOPULL);
	scu_pinmux(SCU_PINMUX_LED2, SCU_GPIO_NOPULL);
	scu_pinmux(SCU_PINMUX_LED3, SCU_GPIO_NOPULL);
	
	scu_pinmux(SCU_PINMUX_EN1V8, SCU_GPIO_NOPULL);
	
	scu_pinmux(SCU_PINMUX_BOOT0, SCU_GPIO_FAST);
	scu_pinmux(SCU_PINMUX_BOOT1, SCU_GPIO_FAST);
	scu_pinmux(SCU_PINMUX_BOOT2, SCU_GPIO_FAST);
	scu_pinmux(SCU_PINMUX_BOOT3, SCU_GPIO_FAST);
	
	/* Configure USB indicators */
	scu_pinmux(SCU_PINMUX_USB_LED0, SCU_CONF_FUNCTION3);
	scu_pinmux(SCU_PINMUX_USB_LED1, SCU_CONF_FUNCTION3);

#ifdef HACKRF_ONE
	/* Configure RF switch control signals */
	scu_pinmux(SCU_HP,             SCU_GPIO_FAST | SCU_CONF_FUNCTION0);
	scu_pinmux(SCU_LP,             SCU_GPIO_FAST | SCU_CONF_FUNCTION0);
	scu_pinmux(SCU_TX_MIX_BP,      SCU_GPIO_FAST | SCU_CONF_FUNCTION0);
	scu_pinmux(SCU_NO_MIX_BYPASS,  SCU_GPIO_FAST | SCU_CONF_FUNCTION0);
	scu_pinmux(SCU_RX_MIX_BP,      SCU_GPIO_FAST | SCU_CONF_FUNCTION0);
	scu_pinmux(SCU_TX_AMP,         SCU_GPIO_FAST | SCU_CONF_FUNCTION0);
	scu_pinmux(SCU_TX,             SCU_GPIO_FAST | SCU_CONF_FUNCTION4);
	scu_pinmux(SCU_MIX_BYPASS,     SCU_GPIO_FAST | SCU_CONF_FUNCTION4);
	scu_pinmux(SCU_RX,             SCU_GPIO_FAST | SCU_CONF_FUNCTION4);
	scu_pinmux(SCU_NO_TX_AMP_PWR,  SCU_GPIO_FAST | SCU_CONF_FUNCTION0);
	scu_pinmux(SCU_AMP_BYPASS,     SCU_GPIO_FAST | SCU_CONF_FUNCTION0);
	scu_pinmux(SCU_RX_AMP,         SCU_GPIO_FAST | SCU_CONF_FUNCTION0);
	scu_pinmux(SCU_NO_RX_AMP_PWR,  SCU_GPIO_FAST | SCU_CONF_FUNCTION0);

	/* Configure RF power supply (VAA) switch */
	scu_pinmux(SCU_NO_VAA_ENABLE,  SCU_GPIO_FAST | SCU_CONF_FUNCTION0);
#endif

	/* Configure all GPIO as Input (safe state) */
	GPIO0_DIR = 0;
	GPIO1_DIR = 0;
	GPIO2_DIR = 0;
	GPIO3_DIR = 0;
	GPIO4_DIR = 0;
	GPIO5_DIR = 0;
	GPIO6_DIR = 0;
	GPIO7_DIR = 0;

	/* Configure GPIO2[1/2/8] (P4_1/2 P6_12) as output. */
	GPIO2_DIR |= (PIN_LED1 | PIN_LED2 | PIN_LED3);

	/* GPIO3[6] on P6_10  as output. */
	GPIO3_DIR |= PIN_EN1V8;

#ifdef HACKRF_ONE
	/* Configure RF switch control signals as outputs */
	GPIO0_DIR |= PIN_AMP_BYPASS;
	GPIO1_DIR |= (PIN_NO_MIX_BYPASS | PIN_RX_AMP | PIN_NO_RX_AMP_PWR);
	GPIO2_DIR |= (PIN_HP | PIN_LP | PIN_TX_MIX_BP | PIN_RX_MIX_BP | PIN_TX_AMP);
	GPIO3_DIR |= PIN_NO_TX_AMP_PWR;
	GPIO5_DIR |= (PIN_TX | PIN_MIX_BYPASS | PIN_RX);

	/*
	 * Safe (initial) switch settings turn off both amplifiers and enable both
	 * amp bypass and mixer bypass.
	 */
	switchctrl_set(SWITCHCTRL_AMP_BYPASS | SWITCHCTRL_MIX_BYPASS);

	/* Configure RF power supply (VAA) switch control signal as output */
	GPIO_DIR(PORT_NO_VAA_ENABLE) |= PIN_NO_VAA_ENABLE;

	/* Safe state: start with VAA turned off: */
	disable_rf_power();
#endif

	/* Configure SSP1 Peripheral (to be moved later in SSP driver) */
	scu_pinmux(SCU_SSP1_MISO, (SCU_SSP_IO | SCU_CONF_FUNCTION5));
	scu_pinmux(SCU_SSP1_MOSI, (SCU_SSP_IO | SCU_CONF_FUNCTION5));
	scu_pinmux(SCU_SSP1_SCK, (SCU_SSP_IO | SCU_CONF_FUNCTION1));
	scu_pinmux(SCU_SSP1_SSEL, (SCU_SSP_IO | SCU_CONF_FUNCTION1));
	
	/* Configure external clock in */
	//scu_pinmux(P4_7, SCU_CLK_IN | SCU_CONF_FUNCTION1);
}

void enable_1v8_power(void) {
	gpio_set(PORT_EN1V8, PIN_EN1V8);
}

void disable_1v8_power(void) {
	gpio_clear(PORT_EN1V8, PIN_EN1V8);
}

#ifdef HACKRF_ONE
void enable_rf_power(void) {
	gpio_clear(PORT_NO_VAA_ENABLE, PIN_NO_VAA_ENABLE);
}

void disable_rf_power(void) {
	gpio_set(PORT_NO_VAA_ENABLE, PIN_NO_VAA_ENABLE);
}

void switchctrl_set(uint8_t ctrl) {
	if (ctrl & SWITCHCTRL_TX) {
		gpio_set(PORT_TX, PIN_TX);
		gpio_clear(PORT_RX, PIN_RX);
	} else {
		gpio_clear(PORT_TX, PIN_TX);
		gpio_set(PORT_RX, PIN_RX);
	}

	if (ctrl & SWITCHCTRL_MIX_BYPASS) {
		gpio_set(PORT_MIX_BYPASS, PIN_MIX_BYPASS);
		gpio_clear(PORT_NO_MIX_BYPASS, PIN_NO_MIX_BYPASS);
		if (ctrl & SWITCHCTRL_TX) {
			gpio_set(PORT_TX_MIX_BP, PIN_TX_MIX_BP);
			gpio_clear(PORT_RX_MIX_BP, PIN_RX_MIX_BP);
		} else {
			gpio_clear(PORT_TX_MIX_BP, PIN_TX_MIX_BP);
			gpio_set(PORT_RX_MIX_BP, PIN_RX_MIX_BP);
		}
	} else {
		gpio_clear(PORT_MIX_BYPASS, PIN_MIX_BYPASS);
		gpio_set(PORT_NO_MIX_BYPASS, PIN_NO_MIX_BYPASS);
		gpio_clear(PORT_TX_MIX_BP, PIN_TX_MIX_BP);
		gpio_clear(PORT_RX_MIX_BP, PIN_RX_MIX_BP);
	}

	if (ctrl & SWITCHCTRL_HP) {
		gpio_set(PORT_HP, PIN_HP);
		gpio_clear(PORT_LP, PIN_LP);
	} else {
		gpio_clear(PORT_HP, PIN_HP);
		gpio_set(PORT_LP, PIN_LP);
	}

	if (ctrl & SWITCHCTRL_AMP_BYPASS) {
		gpio_set(PORT_AMP_BYPASS, PIN_AMP_BYPASS);
		gpio_clear(PORT_TX_AMP, PIN_TX_AMP);
		gpio_set(PORT_NO_TX_AMP_PWR, PIN_NO_TX_AMP_PWR);
		gpio_clear(PORT_RX_AMP, PIN_RX_AMP);
		gpio_set(PORT_NO_RX_AMP_PWR, PIN_NO_RX_AMP_PWR);
	} else if (ctrl & SWITCHCTRL_TX) {
		gpio_clear(PORT_AMP_BYPASS, PIN_AMP_BYPASS);
		gpio_set(PORT_TX_AMP, PIN_TX_AMP);
		gpio_clear(PORT_NO_TX_AMP_PWR, PIN_NO_TX_AMP_PWR);
		gpio_clear(PORT_RX_AMP, PIN_RX_AMP);
		gpio_set(PORT_NO_RX_AMP_PWR, PIN_NO_RX_AMP_PWR);
	} else {
		gpio_clear(PORT_AMP_BYPASS, PIN_AMP_BYPASS);
		gpio_clear(PORT_TX_AMP, PIN_TX_AMP);
		gpio_set(PORT_NO_TX_AMP_PWR, PIN_NO_TX_AMP_PWR);
		gpio_set(PORT_RX_AMP, PIN_RX_AMP);
		gpio_clear(PORT_NO_RX_AMP_PWR, PIN_NO_RX_AMP_PWR);
	}

	/*
	 * These normally shouldn't be set post-Jawbreaker, but they can be
	 * used to explicitly turn off power to the amplifiers while AMP_BYPASS
	 * is unset:
	 */
	if (ctrl & SWITCHCTRL_NO_TX_AMP_PWR)
		gpio_set(PORT_NO_TX_AMP_PWR, PIN_NO_TX_AMP_PWR);
	if (ctrl & SWITCHCTRL_NO_RX_AMP_PWR)
		gpio_set(PORT_NO_RX_AMP_PWR, PIN_NO_RX_AMP_PWR);
}
#endif
