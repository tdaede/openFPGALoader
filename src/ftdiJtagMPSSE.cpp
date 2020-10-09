/*
 * Copyright (C) 2020 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <libusb.h>
#include <stdio.h>
#include <string.h>

#include <iostream>
#include <map>
#include <vector>
#include <string>

#include "ftdiJtagMPSSE.hpp"
#include "ftdipp_mpsse.hpp"

using namespace std;

#define DEBUG 0

#ifdef DEBUG
#define display(...) \
	do { \
		if (_verbose) fprintf(stdout, __VA_ARGS__); \
	}while(0)
#else
#define display(...) do {}while(0)
#endif

FtdiJtagMPSSE::FtdiJtagMPSSE(const FTDIpp_MPSSE::mpsse_bit_config &cable,
			string dev, const string &serial, uint32_t clkHZ, bool verbose):
			FTDIpp_MPSSE(cable, dev, serial, clkHZ, verbose), _ch552WA(false)
{
	init_internal(cable);
}

FtdiJtagMPSSE::~FtdiJtagMPSSE()
{
	int read;
	/* Before shutdown, we must wait until everything is shifted out
	 * Do this by temporary enabling loopback mode, write something
	 * and wait until we can read it back
	 */
	static unsigned char tbuf[16] = { SET_BITS_LOW, 0xff, 0x00,
		SET_BITS_HIGH, 0xff, 0x00,
		LOOPBACK_START,
		MPSSE_DO_READ |
		MPSSE_DO_WRITE | MPSSE_WRITE_NEG | MPSSE_LSB,
		0x04, 0x00,
		0xaa, 0x55, 0x00, 0xff, 0xaa,
		LOOPBACK_END
	};
	mpsse_store(tbuf, 16);
	read = mpsse_read(tbuf, 5);
	if (read != 5)
		fprintf(stderr,
			"Loopback failed, expect problems on later runs %d\n", read);
}

void FtdiJtagMPSSE::init_internal(const FTDIpp_MPSSE::mpsse_bit_config &cable)
{
	/* search for iProduct -> need to have
	 * ftdi->usb_dev (libusb_device_handler) -> libusb_device ->
	 * libusb_device_descriptor
	 */
	struct libusb_device * usb_dev = libusb_get_device(_ftdi->usb_dev);
	struct libusb_device_descriptor usb_desc;
	unsigned char iProduct[200];
	libusb_get_device_descriptor(usb_dev, &usb_desc);
	libusb_get_string_descriptor_ascii(_ftdi->usb_dev, usb_desc.iProduct,
		iProduct, 200);

	display("iProduct : %s\n", iProduct);

	if (!strncmp((const char *)iProduct, "Sipeed-Debug", 12)) {
		_ch552WA = true;
	}

	display("%x\n", cable.bit_low_val);
	display("%x\n", cable.bit_low_dir);
	display("%x\n", cable.bit_high_val);
	display("%x\n", cable.bit_high_dir);

	init(5, 0xfb, BITMODE_MPSSE, (FTDIpp_MPSSE::mpsse_bit_config &)cable);
}

int FtdiJtagMPSSE::writeTMS(uint8_t *tms, int len, bool flush_buffer)
{
	(void) flush_buffer;
	display("%s %d %d\n", __func__, len, (len/8)+1);

	if (len == 0)
		return 0;

	int xfer = len;
	int iter = _buffer_size / 3;
	int offset = 0, pos = 0;

	uint8_t buf[3]= {static_cast<unsigned char>(MPSSE_WRITE_TMS | MPSSE_LSB |
						MPSSE_BITMODE | MPSSE_WRITE_NEG),
						0, 0};
	while (xfer > 0) {
		int bit_to_send = (xfer > 6) ? 6 : xfer;
		buf[1] = bit_to_send-1;
		buf[2] = 0x80;

		for (int i = 0; i < bit_to_send; i++, offset++) {
			buf[2] |=
			(((tms[offset >> 3] & (1 << (offset & 0x07))) ? 1 : 0) << i);
		}
		pos+=3;

		mpsse_store(buf, 3);
		if (pos == iter * 3) {
			pos = 0;
			if (mpsse_write() < 0)
				printf("writeTMS: error\n");

			if (_ch552WA) {
				uint8_t c[len/8+1];
				int ret = ftdi_read_data(_ftdi, c, len/8+1);
				if (ret != 0) {
					printf("ret : %d\n", ret);
				}
			}
		}
		xfer -= bit_to_send;
	}
	mpsse_write();
	if (_ch552WA) {
		uint8_t c[len/8+1];
		ftdi_read_data(_ftdi, c, len/8+1);
	}

	return len;
}

/* need a WA for ch552 */
int FtdiJtagMPSSE::toggleClk(uint8_t tms, uint8_t tdi, uint32_t clk_len)
{
	(void) tdi;
	int ret;
	uint32_t len = clk_len;

	/* clk ouput without data xfer is only supported
	 * with 2232H, 4242H & 232H
	 */

	if (_ftdi->type == TYPE_2232H || _ftdi->type == TYPE_4232H ||
				_ftdi->type == TYPE_232H) {
		uint8_t buf[] = {static_cast<uint8_t>(0x8f), 0, 0};
		if (clk_len > 8) {
			buf[1] = ((len / 8)     ) & 0xff;
			buf[2] = ((len / 8) >> 8) & 0xff;
			mpsse_store(buf, 3);
			ret = mpsse_write();
			if (ret < 0)
				return ret;
			len %= 8;
		}

		if (len > 0) {
			buf[0] = 0x8E;
			buf[1] = len - 1;
			mpsse_store(buf, 2);
			ret = mpsse_write();
			if (ret < 0)
				return ret;
		}
		ret = clk_len;
	} else {
		int byteLen = (len+7)/8;
		uint8_t buf_tms[byteLen];
		memset(buf_tms, (tms) ? 0xff : 0x00, byteLen);
		ret = writeTMS(buf_tms, len, true);
	}

	return ret;
}

int FtdiJtagMPSSE::flush()
{
	return mpsse_write();
}

int FtdiJtagMPSSE::writeTDI(uint8_t *tdi, uint8_t *tdo, uint32_t len, bool last)
{
	/* 3 possible case :
	 *  - n * 8bits to send -> use byte command
	 *  - less than 8bits   -> use bit command
	 *  - last bit to send  -> sent in conjunction with TMS
	 */
	int tx_buff_size = mpsse_get_buffer_size();
	int real_len = (last) ? len - 1 : len;  // if its a buffer in a big send send len
						// else supress last bit -> with TMS
	int nb_byte = real_len >> 3;    // number of byte to send
	int nb_bit = (real_len & 0x07); // residual bits
	int xfer = tx_buff_size - 3;
	unsigned char c[len];
	unsigned char *rx_ptr = (unsigned char *)tdo;
	unsigned char *tx_ptr = (unsigned char *)tdi;
	unsigned char tx_buf[3] = {(unsigned char)(MPSSE_LSB |
						((tdi) ? (MPSSE_DO_WRITE | MPSSE_WRITE_NEG) : 0) |
						((tdo) ? MPSSE_DO_READ : 0)),
						static_cast<unsigned char>((xfer - 1) & 0xff),       // low
						static_cast<unsigned char>((((xfer - 1) >> 8) & 0xff))}; // high

	display("%s len : %d %d %d %d\n", __func__, len, real_len, nb_byte,
		nb_bit);

	if ((nb_byte * 8) + nb_bit != real_len) {
		printf("pas cool\n");
		throw std::exception();
	}

	while (nb_byte != 0) {
		int xfer_len = (nb_byte > xfer) ? xfer : nb_byte;
		tx_buf[1] = (((xfer_len - 1)     ) & 0xff);  // low
		tx_buf[2] = (((xfer_len - 1) >> 8) & 0xff);  // high
		mpsse_store(tx_buf, 3);
		if (tdi) {
			mpsse_store(tx_ptr, xfer_len);
			tx_ptr += xfer_len;
		}
		if (tdo) {
			mpsse_read(rx_ptr, xfer_len);
			rx_ptr += xfer_len;
		} else if (_ch552WA) {
			mpsse_write();
			ftdi_read_data(_ftdi, c, xfer_len);
		} else {
			mpsse_write();
		}
		nb_byte -= xfer_len;
	}

	unsigned char last_bit = (tdi) ? *tx_ptr : 0;

	if (nb_bit != 0) {
		display("%s read/write %d bit\n", __func__, nb_bit);
		tx_buf[0] |= MPSSE_BITMODE;
		tx_buf[1] = nb_bit - 1;
		mpsse_store(tx_buf, 2);
		if (tdi) {
			display("%s last_bit %x size %d\n", __func__, last_bit, nb_bit-1);
			mpsse_store(last_bit);
		}
		if (tdo) {
			mpsse_read(rx_ptr, 1);
			/* realign we have read nb_bit
			 * since LSB add bit by the left and shift
			 * we need to complete shift
			 */
			*rx_ptr >>= (8 - nb_bit);
			display("%s %x\n", __func__, *rx_ptr);
		} else if (_ch552WA) {
			mpsse_write();
			ftdi_read_data(_ftdi, c, nb_bit);
		} else {
			mpsse_write();
		}
	}

	/* display : must be dropped */
	if (_verbose && tdo) {
		display("\n");
		for (int i = (len / 8) - 1; i >= 0; i--)
			display("%x ", (unsigned char)tdo[i]);
		display("\n");
	}

	if (last == 1) {
		last_bit = (tdi)? (*tx_ptr & (1 << nb_bit)) : 0;

		display("%s move to EXIT1_xx and send last bit %x\n", __func__, (last_bit?0x81:0x01));
		/* write the last bit in conjunction with TMS */
		tx_buf[0] = MPSSE_WRITE_TMS | MPSSE_LSB | MPSSE_BITMODE | MPSSE_WRITE_NEG |
					((tdo) ? MPSSE_DO_READ : 0);
		tx_buf[1] = 0x0;  // send 1bit
		tx_buf[2] = ((last_bit) ? 0x81 : 0x01);  // we know in TMS tdi is bit 7
							// and to move to EXIT_XR TMS = 1
		mpsse_store(tx_buf, 3);
		if (tdo) {
			unsigned char c;
			mpsse_read(&c, 1);
			/* in this case for 1 one it's always bit 7 */
			*rx_ptr |= ((c & 0x80) << (7 - nb_bit));
			display("%s %x\n", __func__, c);
		} else if (_ch552WA) {
			mpsse_write();
			ftdi_read_data(_ftdi, c, 1);
		} else {
			mpsse_write();
		}
	}

	return 0;
}
