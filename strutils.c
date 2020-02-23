/* String utils */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "strutils.h"

/* ------------------------------------------------------------------------ */
/* Decompose a URL into its components. */

void split_url(struct url_info *ui, char *url) {
    char *p = url;
    char *q;

    memset(ui, 0, sizeof *ui);

    q = strstr(p, "://");

    ui->scheme = p;
    *q = '\0';
    p = q+3;
    for (int i = 0; ui->scheme[i]; i++) {
        ui->scheme[i] = tolower(ui->scheme[i]);
    }

    q = strchr(p, '/');
    if (q) {
	*q = '/';
	ui->path = q;
	q = strchr(q, '#');
	if (q)
	    *q = '\0';
    } else {
	ui->path = "/";
    }

    ui->hostn = p;
    q = strchr(p, ':');
    if (q) {
	*q = '\0';
	ui->port = atoi(q+1);
    }
    if (ui->port == 0) {
        if (strcmp(ui->scheme,"http") == 0) {
            ui->port = 80;
        }
        if (strcmp(ui->scheme,"https") == 0) {
            ui->port = 443;
        }
    }
}
/* ------------------------------------------------------------------------ */
// convert a hex string such as "A489B1" into an array like [0xA4, 0x89, 0xB1].

char nibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';

  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;

  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;

  return 0;  // Not a valid hexadecimal character
}

void hexToBytes(char *byteArray, const char *hexString) {
  char oddLength = strlen(hexString) & 1;

  char currentByte = 0;
  char byteIndex = 0;

  for (char charIndex = 0; charIndex < strlen(hexString); charIndex++) {
    char oddCharIndex = charIndex & 1;

    if (oddLength) {
        if (oddCharIndex) {
            currentByte = nibble(hexString[charIndex]) << 4;
        } else {
            currentByte |= nibble(hexString[charIndex]);
            byteArray[byteIndex++] = currentByte;
            currentByte = 0;
        }
    } else {
        if (!oddCharIndex) {
            currentByte = nibble(hexString[charIndex]) << 4;
        } else {
            currentByte |= nibble(hexString[charIndex]);
            byteArray[byteIndex++] = currentByte;
            currentByte = 0;
        }
    }
  }
}
/* ------------------------------------------------------------------------ */
const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
/* ------------------------------------------------------------------------ */

size_t b64_encoded_size(size_t inlen) {
    size_t ret;

    ret = inlen;
    if (inlen % 3 != 0) {
        ret += 3 - (inlen % 3);
    }
    ret /= 3;
    ret *= 4;

    return ret;
}
/* ------------------------------------------------------------------------ */
char b64_encode(const char *in, char *out, size_t len) {
    size_t  elen;
    size_t  i;
    size_t  j;
    size_t  v;

    if (len == 0) return NULL;

    elen = b64_encoded_size(len);
    out[elen] = '\0';

    for (i=0, j=0; i<len; i+=3, j+=4) {
        v = in[i];
        v = i+1 < len ? v << 8 | in[i+1] : v << 8;
        v = i+2 < len ? v << 8 | in[i+2] : v << 8;

        out[j]   = b64chars[(v >> 18) & 0x3F];
        out[j+1] = b64chars[(v >> 12) & 0x3F];
        if (i+1 < len) {
            out[j+2] = b64chars[(v >> 6) & 0x3F];
        } else {
            out[j+2] = '=';
        }
        if (i+2 < len) {
            out[j+3] = b64chars[v & 0x3F];
        } else {
            out[j+3] = '=';
        }
    }
}

/* ------------------------------------------------------------------------ */
// in-place trim whitespace from right
void trimr(char *str) {
    while (isspace(str[strlen(str)-1]) && strlen(str) > 0) str[strlen(str)-1] = 0;
}
/* ------------------------------------------------------------------------ */
