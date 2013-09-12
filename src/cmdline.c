/*
 * Copyright (c) Michael Tharp <gxti@partiallystapled.com>
 *
 * This file is distributed under the terms of the MIT License.
 * See the LICENSE file at the top of this tree, or if it is missing a copy can
 * be found at http://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "common.h"
#include "cmdline.h"
#include "init.h"
//#include "lwip/def.h"
//#include "lwipthread.h"
#include "eeprom.h"
#include "uptime.h"
#include "util.h"
//#include "version.h"
#define VERSION "1.2.3.4"


uint8_t cl_enabled;
serial_t *cl_out;

static char cl_buf[64];
static char fmt_buf[64];
static uint8_t cl_count;

static void uartPrint(const char *value);
static void uart_printf(const char *fmt, ...);

static void cliDefaults(char *cmdline);
static void cliExit(char *cmdline);
static void cliHelp(char *cmdline);
static void cliInfo(char *cmdline);
static void cliSave(char *cmdline);
static void cliSet(char *cmdline);
static void cliUptime(char *cmdline);
static void cliVersion(char *cmdline);

static void cli_print_hwaddr(void);


typedef struct {
	char *name;
	char *param;
	void (*func)(char *cmdline);
} clicmd_t;


typedef enum {
	VAR_UINT32,
	VAR_BOOL,
	VAR_IP4
} vartype_e;


typedef struct {
	const char *name;
	const uint8_t type; /* vartype_e */
	void *ptr;
} clivalue_t;

static void cliSetVar(const clivalue_t *var, const char *valstr);
static void cliPrintVar(const clivalue_t *var, uint8_t full);

/* Keep sorted */
const clicmd_t cmd_table[] = {
	{ "defaults", "reset to factory defaults and reboot", cliDefaults },
	{ "exit", "leave command mode", cliExit },
	{ "help", "", cliHelp },
	{ "info", "show runtime information", cliInfo },
	{ "save", "save changes and reboot", cliSave },
	{ "set", "name=value or blank or * for list", cliSet },
	{ "uptime", "show the system uptime", cliUptime },
	{ "version", "show version", cliVersion },
};
#define CMD_COUNT (sizeof(cmd_table) / sizeof(cmd_table[0]))

const clivalue_t value_table[] = {
	{ "gps_baud_rate", VAR_UINT32, &cfg.gps_baud_rate },
	{ "ip_addr", VAR_IP4, &cfg.ip_addr },
	{ "ip_gateway", VAR_IP4, &cfg.ip_gateway },
	{ "ip_netmask", VAR_IP4, &cfg.ip_netmask },
};
#define VALUE_COUNT (sizeof(value_table) / sizeof(value_table[0]))


/* Initialization and UART handling */

void
cli_set_output(serial_t *output) {
	cl_out = output;
}

static void
uartPrint(const char *value) {
	serial_puts(cl_out, value);
}


static void
uart_printf(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(fmt_buf, sizeof(fmt_buf), fmt, ap);
	va_end(ap);
	serial_puts(cl_out, fmt_buf);
}


/* Command-line helpers */

static void
cliPrompt(void) {
	cl_count = 0;
	cl_enabled = 1;
	uartPrint("\r\n# ");
}


static int
cliCompare(const void *a, const void *b) {
	const clicmd_t *ca = a, *cb = b;
	return strncasecmp(ca->name, cb->name, strlen(cb->name));
}


static void
cliPrintVar(const clivalue_t *var, uint8_t full) {
	switch (var->type) {
	case VAR_UINT32:
		uart_printf("%u", *(uint32_t*)var->ptr);
		break;
	case VAR_BOOL:
		uart_printf("%u", !!*(uint8_t*)var->ptr);
		break;
	case VAR_IP4:
		{
			uint8_t *addr = (uint8_t*)var->ptr;
			uart_printf("%d.%d.%d.%d", addr[0], addr[1], addr[2], addr[3]);
			break;
		}
	}
}


static void
cliSetVar(const clivalue_t *var, const char *str) {
	uint32_t val = 0;
	uint8_t val2 = 0;
	switch (var->type) {
	case VAR_UINT32:
		*(uint32_t*)var->ptr = atoi_decimal(str);
		break;
	case VAR_BOOL:
		*(uint8_t*)var->ptr = !!atoi_decimal(str);
		break;
	case VAR_IP4:
		while (*str) {
			if (*str == '.') {
				val = (val << 8) | val2;
				val2 = 0;
			} else if (*str >= '0' && *str <= '9') {
				val2 = val2 * 10 + (*str - '0');
			}
			str++;
		}
		val = (val << 8) | val2;
		//*(uint32_t*)var->ptr = lwip_htonl(val);
		break;
	}
}


void
cli_feed(char c) {
	if (cl_enabled == 0 && c != '\r' && c != '\n') {
		return;
	}
	cl_enabled = 1;

	if (c == '\t' || c == '?') {
		/* Tab completion */
		/* TODO */
	} else if (!cl_count && c == 4) {
		/* EOF */
		cliExit(cl_buf);
	} else if (c == 12) {
		/* Clear screen */
		uartPrint("\033[2J\033[1;1H");
		cliPrompt();
	} else if (c == '\n' || c == '\r') {
		if (cl_count) {
			/* Enter pressed */
			clicmd_t *cmd = NULL;
			clicmd_t target;
			uartPrint("\r\n");
			cl_buf[cl_count] = 0;

			target.name = cl_buf;
			target.param = NULL;
			cmd = bsearch(&target, cmd_table, CMD_COUNT, sizeof(cmd_table[0]),
					cliCompare);
			if (cmd) {
				cmd->func(cl_buf + strlen(cmd->name) + 1);
			} else {
				uartPrint("ERR: Unknown command, try 'help'\r\n");
			}
			memset(cl_buf, 0, sizeof(cl_buf));
		} else if (c == '\n' && cl_buf[0] == '\r') {
			/* Ignore \n after \r */
			return;
		}
		if (cl_enabled) {
			cliPrompt();
		}
		cl_buf[0] = c;
	} else if (c == 8 || c == 127) {
		/* Backspace */
		if (cl_count) {
			cl_buf[--cl_count] = 0;
			uartPrint("\010 \010");
		}
	} else if (cl_count < sizeof(cl_buf) && c >= 32 && c <= 126) {
		if (!cl_count && c == 32) {
			return;
		}
		cl_buf[cl_count++] = c;
		serial_putc(cl_out, c);
	}
}


/* Command implementation */

static void
cliWriteConfig(void) {
	int16_t result;
	uartPrint("Writing EEPROM...\r\n");
	result = eeprom_write_cfg();
	if (result == EERR_TIMEOUT) {
		uartPrint("ERROR: timeout while writing EEPROM\r\n");
	} else if (result == EERR_NACK) {
		uartPrint("ERROR: EEPROM is faulty or missing\r\n");
	} else if (result == EERR_FAULT) {
		uartPrint("ERROR: EEPROM is faulty\r\n");
	} else if (result != EERR_OK) {
		uartPrint("FAIL: unable to write EEPROM\r\n");
	} else {
		uartPrint("OK\r\n");
		CoTickDelay(S2ST(1));
		/* reset */
		SCB->AIRCR = 0x05FA0000 | 0x04;
	}
}


static void
cliDefaults(char *cmdline) {
	memset(cfg_bytes, 0, EEPROM_CFG_SIZE);
	cfg.version = CFG_VERSION;
	cliWriteConfig();
}


static void
cliExit(char *cmdline) {
	cl_count = 0;
	cl_enabled = 0;
	uartPrint("Exiting cmdline mode.\r\n"
			"Configuration changes have not been saved.\r\n"
			"Press Enter to enable cmdline.\r\n");
}


static void
cliHelp(char *cmdline) {
	uint8_t i;
	uartPrint("Available commands:\r\n");
	for (i = 0; i < CMD_COUNT; i++) {
		uart_printf("%s\t%s\r\n", cmd_table[i].name, cmd_table[i].param);
	}
}


static void
cliInfo(char *cmdline) {
	cliVersion(NULL);
	cli_print_hwaddr();
	//print_netif(CHB);
	cliUptime(NULL);
	uart_printf("System clock:   %d Hz (nominal)\r\n", (int)system_frequency);
}


static void
cliSave(char *cmdline) {
	cliWriteConfig();
}


static void
cliSet(char *cmdline) {
	uint32_t i, len;
	const clivalue_t *val;
	char *eqptr = NULL;

	len = strlen(cmdline);
	if (len == 0 || (len == 1 && cmdline[0] == '*')) {
		uartPrint("Current settings:\r\n");
		for (i = 0; i < VALUE_COUNT; i++) {
			val = &value_table[i];
			uart_printf("%s = ", value_table[i].name);
			cliPrintVar(val, len);
			uartPrint("\r\n");
		}
	} else if ((eqptr = strstr(cmdline, "="))) {
		eqptr++;
		len--;
		while (*eqptr == ' ') {
			eqptr++;
			len--;
		}
		for (i = 0; i < VALUE_COUNT; i++) {
			val = &value_table[i];
			if (strncasecmp(cmdline, value_table[i].name,
						strlen(value_table[i].name)) == 0) {
				cliSetVar(val, eqptr);
				uart_printf("%s set to ", value_table[i].name);
				cliPrintVar(val, 0);
				return;
			}
		}
		uartPrint("ERR: Unknown variable name\r\n");
	}
}


static void
cliUptime(char *cmdline) {
	uartPrint("Uptime:         ");
	uartPrint(uptime_format());
	uartPrint("\r\n");
}


static void
cliVersion(char *cmdline) {
	uartPrint(
		"Hardware:       " BOARD_REV "\r\n"
		"Software:       " VERSION "\r\n");
}


static void
cli_print_hwaddr(void) {
	uartPrint("MAC Address:    ");
	//print_hwaddr(CHB);
	uartPrint("\r\n");
}


void
cli_banner(void) {
	uartPrint("\r\n\r\nLaureline GPS NTP Server\r\n");
	cliVersion(NULL);
	cli_print_hwaddr();
	uartPrint("\r\nPress Enter to enable command-line\r\n");
}