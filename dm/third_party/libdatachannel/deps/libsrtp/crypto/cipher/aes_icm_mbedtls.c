/*
 * aes_icm_mbedtls.c
 *
 * AES Integer Counter Mode
 *
 * YongCheng Yang
 */

/*
 *
 * Copyright (c) 2013-2017, Cisco Systems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 *   Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following
 *   disclaimer in the documentation and/or other materials provided
 *   with the distribution.
 *
 *   Neither the name of the Cisco Systems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
/* build_info.h was added in mbedtls 3.0; mbedtls 2.x ships version.h only.
 * Use __has_include so the gate still resolves on 2.x (the legacy code path).
 */
#if defined(__has_include) && __has_include(<mbedtls/build_info.h>)
#include <mbedtls/build_info.h>
#else
#include <mbedtls/version.h>
#endif
#if MBEDTLS_VERSION_MAJOR >= 4
#include <psa/crypto_types.h>
#include <psa/crypto.h>
#else
#include <mbedtls/aes.h>
#endif
#include "aes_icm_ext.h"
#include "crypto_types.h"
#include "err.h" /* for srtp_debug */
#include "alloc.h"
#include "cipher_types.h"
#include "cipher_test_cases.h"

srtp_debug_module_t srtp_mod_aes_icm = {
    0,                /* debugging is off by default */
    "aes icm mbedtls" /* printable module name       */
};

/*
 * integer counter mode works as follows:
 *
 * https://tools.ietf.org/html/rfc3711#section-4.1.1
 *
 * E(k, IV) || E(k, IV + 1 mod 2^128) || E(k, IV + 2 mod 2^128) ...
 * IV = (k_s * 2^16) XOR (SSRC * 2^64) XOR (i * 2^16)
 *
 * IV SHALL be defined by the SSRC, the SRTP packet index i,
 * and the SRTP session salting key k_s.
 *
 * SSRC: 32bits.
 * Sequence number: 16bits.
 * nonce is 64bits. .
 * packet index = ROC || SEQ. (ROC: Rollover counter)
 *
 * 16 bits
 * <----->
 * +------+------+------+------+------+------+------+------+
 * |           nonce           |    packet index    |  ctr |---+
 * +------+------+------+------+------+------+------+------+   |
 *                                                             |
 * +------+------+------+------+------+------+------+------+   v
 * |                      salt                      |000000|->(+)
 * +------+------+------+------+------+------+------+------+   |
 *                                                             |
 *                                                        +---------+
 *                                                        | encrypt |
 *                                                        +---------+
 *                                                             |
 * +------+------+------+------+------+------+------+------+   |
 * |                    keystream block                    |<--+
 * +------+------+------+------+------+------+------+------+
 *
 * All fields are big-endian
 *
 * ctr is the block counter, which increments from zero for
 * each packet (16 bits wide)
 *
 * packet index is distinct for each packet (48 bits wide)
 *
 * nonce can be distinct across many uses of the same key, or
 * can be a fixed value per key, or can be per-packet randomness
 * (64 bits)
 *
 */

/*
 * This function allocates a new instance of this crypto engine.
 * The key_len parameter should be one of 30, 38, or 46 for
 * AES-128, AES-192, and AES-256 respectively.  Note, this key_len
 * value is inflated, as it also accounts for the 112 bit salt
 * value.  The tlen argument is for the AEAD tag length, which
 * isn't used in counter mode.
 */
static srtp_err_status_t srtp_aes_icm_mbedtls_alloc(srtp_cipher_t **c,
                                                    int key_len,
                                                    int tlen)
{
    srtp_aes_icm_ctx_t *icm;
    (void)tlen;

    debug_print(srtp_mod_aes_icm, "allocating cipher with key length %d",
                key_len);

    /*
     * Verify the key_len is valid for one of: AES-128/192/256
     */
    if (key_len != SRTP_AES_ICM_128_KEY_LEN_WSALT &&
        key_len != SRTP_AES_ICM_192_KEY_LEN_WSALT &&
        key_len != SRTP_AES_ICM_256_KEY_LEN_WSALT) {
        return srtp_err_status_bad_param;
    }

    /* allocate memory a cipher of type aes_icm */
    *c = (srtp_cipher_t *)srtp_crypto_alloc(sizeof(srtp_cipher_t));
    if (*c == NULL) {
        return srtp_err_status_alloc_fail;
    }

    icm = (srtp_aes_icm_ctx_t *)srtp_crypto_alloc(sizeof(srtp_aes_icm_ctx_t));
    if (icm == NULL) {
        srtp_crypto_free(*c);
        *c = NULL;
        return srtp_err_status_alloc_fail;
    }

#if MBEDTLS_VERSION_MAJOR >= 4
    icm->ctx =
        (psa_aes_icm_ctx_t *)srtp_crypto_alloc(sizeof(psa_aes_icm_ctx_t));
    if (icm->ctx == NULL) {
        srtp_crypto_free(icm);
        srtp_crypto_free(*c);
        *c = NULL;
        return srtp_err_status_alloc_fail;
    }

    icm->ctx->key_id = PSA_KEY_ID_NULL;
    icm->ctx->op = psa_cipher_operation_init();
#else
    icm->ctx =
        (mbedtls_aes_context *)srtp_crypto_alloc(sizeof(mbedtls_aes_context));
    if (icm->ctx == NULL) {
        srtp_crypto_free(icm);
        srtp_crypto_free(*c);
        *c = NULL;
        return srtp_err_status_alloc_fail;
    }

    mbedtls_aes_init(icm->ctx);
#endif

    /* set pointers */
    (*c)->state = icm;

    /* setup cipher parameters */
    switch (key_len) {
    case SRTP_AES_ICM_128_KEY_LEN_WSALT:
        (*c)->algorithm = SRTP_AES_ICM_128;
        (*c)->type = &srtp_aes_icm_128;
        icm->key_size = SRTP_AES_128_KEY_LEN;
        break;
    case SRTP_AES_ICM_192_KEY_LEN_WSALT:
        (*c)->algorithm = SRTP_AES_ICM_192;
        (*c)->type = &srtp_aes_icm_192;
        icm->key_size = SRTP_AES_192_KEY_LEN;
        break;
    case SRTP_AES_ICM_256_KEY_LEN_WSALT:
        (*c)->algorithm = SRTP_AES_ICM_256;
        (*c)->type = &srtp_aes_icm_256;
        icm->key_size = SRTP_AES_256_KEY_LEN;
        break;
    }

    /* set key size        */
    (*c)->key_len = key_len;

    return srtp_err_status_ok;
}

/*
 * This function deallocates an instance of this engine
 */
static srtp_err_status_t srtp_aes_icm_mbedtls_dealloc(srtp_cipher_t *c)
{
    srtp_aes_icm_ctx_t *ctx;

    if (c == NULL) {
        return srtp_err_status_bad_param;
    }

    /*
     * Free the aes context
     */
    ctx = (srtp_aes_icm_ctx_t *)c->state;
    if (ctx != NULL) {
#if MBEDTLS_VERSION_MAJOR >= 4
        psa_cipher_abort(&ctx->ctx->op);
        psa_destroy_key(ctx->ctx->key_id);
#else
        mbedtls_aes_free(ctx->ctx);
#endif
        srtp_crypto_free(ctx->ctx);
        /* zeroize the key material */
        octet_string_set_to_zero(ctx, sizeof(srtp_aes_icm_ctx_t));
        srtp_crypto_free(ctx);
    }

    /* free memory */
    srtp_crypto_free(c);

    return srtp_err_status_ok;
}

static srtp_err_status_t srtp_aes_icm_mbedtls_context_init(void *cv,
                                                           const uint8_t *key)
{
    srtp_aes_icm_ctx_t *c = (srtp_aes_icm_ctx_t *)cv;
    uint32_t key_size_in_bits = (c->key_size << 3);
#if MBEDTLS_VERSION_MAJOR >= 4
    psa_status_t status = PSA_SUCCESS;

    /* psa_crypto_init() is idempotent and required before any PSA use. */
    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        debug_print(srtp_mod_aes_icm, "psa_crypto_init failed: %d", status);
        return srtp_err_status_init_fail;
    }
#else
    int errcode = 0;
#endif

    /*
     * set counter and initial values to 'offset' value, being careful not to
     * go past the end of the key buffer
     */
    v128_set_to_zero(&c->counter);
    v128_set_to_zero(&c->offset);
    memcpy(&c->counter, key + c->key_size, SRTP_SALT_LEN);
    memcpy(&c->offset, key + c->key_size, SRTP_SALT_LEN);

    /* force last two octets of the offset to zero (for srtp compatibility) */
    c->offset.v8[SRTP_SALT_LEN] = c->offset.v8[SRTP_SALT_LEN + 1] = 0;
    c->counter.v8[SRTP_SALT_LEN] = c->counter.v8[SRTP_SALT_LEN + 1] = 0;
    debug_print(srtp_mod_aes_icm, "key:  %s",
                srtp_octet_string_hex_string(key, c->key_size));
    debug_print(srtp_mod_aes_icm, "offset: %s", v128_hex_string(&c->offset));

    switch (c->key_size) {
    case SRTP_AES_256_KEY_LEN:
    case SRTP_AES_192_KEY_LEN:
    case SRTP_AES_128_KEY_LEN:
        break;
    default:
        return srtp_err_status_bad_param;
        break;
    }

#if MBEDTLS_VERSION_MAJOR >= 4
    {
        psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&attr, key_size_in_bits);
        psa_set_key_usage_flags(&attr,
                                PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
        /* SRTP AES-ICM spec only allows the low 16 bits of the counter to
         * vary per packet (top 14 bytes are the salted nonce, see RFC 3711).
         * PSA_ALG_CTR increments the full 128-bit counter, identical to the
         * legacy mbedtls_aes_crypt_ctr path. For SRTP packet sizes
         * (< 2^16 blocks = 1 MiB) the keystreams agree, so all conformant
         * RTP/RTCP payloads are correct. Buffers > 1 MiB are not
         * SRTP-conformant on either backend. */
        psa_set_key_algorithm(&attr, PSA_ALG_CTR);

        if (c->ctx->key_id != PSA_KEY_ID_NULL) {
            /* Abort any in-flight cipher op before destroying its key:
             * a leftover op from a prior set_iv() still references key_id,
             * and on PSA implementations that eagerly invalidate op key
             * handles, destroy_key would leave the op in a bad state. */
            psa_cipher_abort(&c->ctx->op);
            c->ctx->op = psa_cipher_operation_init();
            psa_destroy_key(c->ctx->key_id);
            c->ctx->key_id = PSA_KEY_ID_NULL;
        }

        status =
            psa_import_key(&attr, key, key_size_in_bits / 8, &(c->ctx->key_id));
        /* Done with the attributes on both success and failure paths. */
        psa_reset_key_attributes(&attr);
        if (status != PSA_SUCCESS) {
            debug_print(srtp_mod_aes_icm, "psa_import_key failed: %d", status);
            return srtp_err_status_init_fail;
        }
    }
#else
    errcode = mbedtls_aes_setkey_enc(c->ctx, key, key_size_in_bits);
    if (errcode != 0) {
        debug_print(srtp_mod_aes_icm, "errCode: %d", errcode);
    }
#endif

    return srtp_err_status_ok;
}

/*
 * aes_icm_set_iv(c, iv) sets the counter value to the exor of iv with
 * the offset
 */
static srtp_err_status_t srtp_aes_icm_mbedtls_set_iv(
    void *cv,
    uint8_t *iv,
    srtp_cipher_direction_t dir)
{
    srtp_aes_icm_ctx_t *c = (srtp_aes_icm_ctx_t *)cv;
    v128_t nonce;
    (void)dir;

    c->nc_off = 0;
    /* set nonce (for alignment) */
    v128_copy_octet_string(&nonce, iv);

    debug_print(srtp_mod_aes_icm, "setting iv: %s", v128_hex_string(&nonce));

    v128_xor(&c->counter, &c->offset, &nonce);

    debug_print(srtp_mod_aes_icm, "set_counter: %s",
                v128_hex_string(&c->counter));

#if MBEDTLS_VERSION_MAJOR >= 4
    {
        psa_status_t status;

        /* Reset any prior in-flight cipher operation. */
        psa_cipher_abort(&c->ctx->op);
        c->ctx->op = psa_cipher_operation_init();

        status =
            psa_cipher_encrypt_setup(&c->ctx->op, c->ctx->key_id, PSA_ALG_CTR);
        if (status != PSA_SUCCESS) {
            debug_print(srtp_mod_aes_icm, "psa_cipher_encrypt_setup failed: %d",
                        status);
            psa_cipher_abort(&c->ctx->op);
            return srtp_err_status_cipher_fail;
        }

        status = psa_cipher_set_iv(&c->ctx->op, c->counter.v8, 16);
        if (status != PSA_SUCCESS) {
            debug_print(srtp_mod_aes_icm, "psa_cipher_set_iv failed: %d",
                        status);
            psa_cipher_abort(&c->ctx->op);
            return srtp_err_status_cipher_fail;
        }
    }
#endif

    return srtp_err_status_ok;
}

/*
 * This function encrypts a buffer using AES CTR mode
 *
 * Parameters:
 *	c	Crypto context
 *	buf	data to encrypt
 *	enc_len	length of encrypt buffer
 */
static srtp_err_status_t srtp_aes_icm_mbedtls_encrypt(void *cv,
                                                      unsigned char *buf,
                                                      unsigned int *enc_len)
{
    srtp_aes_icm_ctx_t *c = (srtp_aes_icm_ctx_t *)cv;

    debug_print(srtp_mod_aes_icm, "rs0: %s", v128_hex_string(&c->counter));

#if MBEDTLS_VERSION_MAJOR >= 4
    {
        psa_status_t status;

        /*
         * libsrtp's AES-ICM interface is incremental: encrypt() may be called
         * multiple times after a single set_iv().  psa_cipher_update is called
         * repeatedly on the same operation handle to match that; the operation
         * is only finished at dealloc/set_iv-reset time via psa_cipher_abort.
         *
         * SRTP always encrypts in place (input buffer == output buffer). The
         * PSA spec permits overlapping in/out buffers, but TF-PSA-Crypto (the
         * crypto backend shipped with ESP-IDF v6 / mbedTLS 4) rejects an
         * in-place psa_cipher_update for AES-CTR on non-block-aligned lengths
         * with PSA_ERROR_INVALID_ARGUMENT. Stage through a bounded stack buffer
         * to keep input and output non-overlapping. AES-CTR is a stream cipher
         * (out_len == in_len, no deferred bytes), so processing in fixed-size,
         * block-aligned chunks preserves the keystream position across calls.
         */
        unsigned char scratch[128]; /* block-aligned staging buffer */
        unsigned int done = 0;
        while (done < *enc_len) {
            size_t out_len = 0;
            size_t chunk = *enc_len - done;
            if (chunk > sizeof(scratch)) {
                chunk = sizeof(scratch);
            }
            status = psa_cipher_update(&c->ctx->op, buf + done, chunk, scratch,
                                       sizeof(scratch), &out_len);
            if (status != PSA_SUCCESS) {
                debug_print(srtp_mod_aes_icm, "psa_cipher_update failed: %d",
                            status);
                psa_cipher_abort(&c->ctx->op);
                return srtp_err_status_cipher_fail;
            }
            /* Stream-cipher invariant: a CTR update never defers bytes. If a
             * backend ever buffered a partial block, the keystream would
             * desync across chunks — fail loudly rather than corrupt data. */
            if (out_len != chunk) {
                debug_print2(srtp_mod_aes_icm,
                             "psa_cipher_update short write: %zu of %zu "
                             "(PSA backend defers stream-cipher output; "
                             "unsupported in this wrapper)",
                             out_len, chunk);
                psa_cipher_abort(&c->ctx->op);
                return srtp_err_status_cipher_fail;
            }
            memcpy(buf + done, scratch, out_len);
            done += (unsigned int)out_len;
        }
    }
#else
    {
        int errCode =
            mbedtls_aes_crypt_ctr(c->ctx, *enc_len, &(c->nc_off), c->counter.v8,
                                  c->stream_block.v8, buf, buf);
        if (errCode != 0) {
            debug_print(srtp_mod_aes_icm, "encrypt error: %d", errCode);
            return srtp_err_status_cipher_fail;
        }
    }
#endif

    return srtp_err_status_ok;
}

/*
 * Name of this crypto engine
 */
static const char srtp_aes_icm_128_mbedtls_description[] =
    "AES-128 counter mode using mbedtls";
static const char srtp_aes_icm_192_mbedtls_description[] =
    "AES-192 counter mode using mbedtls";
static const char srtp_aes_icm_256_mbedtls_description[] =
    "AES-256 counter mode using mbedtls";

/*
 * This is the function table for this crypto engine.
 * note: the encrypt function is identical to the decrypt function
 */
const srtp_cipher_type_t srtp_aes_icm_128 = {
    srtp_aes_icm_mbedtls_alloc,           /* */
    srtp_aes_icm_mbedtls_dealloc,         /* */
    srtp_aes_icm_mbedtls_context_init,    /* */
    0,                                    /* set_aad */
    srtp_aes_icm_mbedtls_encrypt,         /* */
    srtp_aes_icm_mbedtls_encrypt,         /* */
    srtp_aes_icm_mbedtls_set_iv,          /* */
    0,                                    /* get_tag */
    srtp_aes_icm_128_mbedtls_description, /* */
    &srtp_aes_icm_128_test_case_0,        /* */
    SRTP_AES_ICM_128                      /* */
};

/*
 * This is the function table for this crypto engine.
 * note: the encrypt function is identical to the decrypt function
 */
const srtp_cipher_type_t srtp_aes_icm_192 = {
    srtp_aes_icm_mbedtls_alloc,           /* */
    srtp_aes_icm_mbedtls_dealloc,         /* */
    srtp_aes_icm_mbedtls_context_init,    /* */
    0,                                    /* set_aad */
    srtp_aes_icm_mbedtls_encrypt,         /* */
    srtp_aes_icm_mbedtls_encrypt,         /* */
    srtp_aes_icm_mbedtls_set_iv,          /* */
    0,                                    /* get_tag */
    srtp_aes_icm_192_mbedtls_description, /* */
    &srtp_aes_icm_192_test_case_0,        /* */
    SRTP_AES_ICM_192                      /* */
};

/*
 * This is the function table for this crypto engine.
 * note: the encrypt function is identical to the decrypt function
 */
const srtp_cipher_type_t srtp_aes_icm_256 = {
    srtp_aes_icm_mbedtls_alloc,           /* */
    srtp_aes_icm_mbedtls_dealloc,         /* */
    srtp_aes_icm_mbedtls_context_init,    /* */
    0,                                    /* set_aad */
    srtp_aes_icm_mbedtls_encrypt,         /* */
    srtp_aes_icm_mbedtls_encrypt,         /* */
    srtp_aes_icm_mbedtls_set_iv,          /* */
    0,                                    /* get_tag */
    srtp_aes_icm_256_mbedtls_description, /* */
    &srtp_aes_icm_256_test_case_0,        /* */
    SRTP_AES_ICM_256                      /* */
};
