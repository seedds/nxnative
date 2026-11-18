/*
 * aes_gcm_mbedtls.c
 *
 * AES Galois Counter Mode
 *
 * YongCheng Yang
 *
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
#include <psa/crypto.h>
#else
#include <mbedtls/gcm.h>
#endif
#include "aes_gcm.h"
#include "alloc.h"
#include "err.h" /* for srtp_debug */
#include "crypto_types.h"
#include "cipher_types.h"
#include "cipher_test_cases.h"

srtp_debug_module_t srtp_mod_aes_gcm = {
    0,                /* debugging is off by default */
    "aes gcm mbedtls" /* printable module name       */
};

/**
 * SRTP IV Formation for AES-GCM
 * https://tools.ietf.org/html/rfc7714#section-8.1
 *   0  0  0  0  0  0  0  0  0  0  1  1
 *   0  1  2  3  4  5  6  7  8  9  0  1
 *  +--+--+--+--+--+--+--+--+--+--+--+--+
 *  |00|00| SSRC      | ROC       | SEQ |---+
 *  +--+--+--+--+--+--+--+--+--+--+--+--+   |
 *  |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+   |
 *  | Encryption Salt                   |->(+)
 *  +--+--+--+--+--+--+--+--+--+--+--+--+   |
 *                                          |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+   |
 *  | Initialization Vector             |<--+
 *  +--+--+--+--+--+--+--+--+--+--+--+--+
 *
 * SRTCP IV Formation for AES-GCM
 * https://tools.ietf.org/html/rfc7714#section-9.1
 *
 */

/*
 * For now we only support 8 and 16 octet tags.  The spec allows for
 * optional 12 byte tag, which may be supported in the future.
 */
#define GCM_IV_LEN 12
#define GCM_AUTH_TAG_LEN 16
#define GCM_AUTH_TAG_LEN_8 8

#define FUNC_ENTRY() debug_print(srtp_mod_aes_gcm, "%s entry", __func__);
/*
 * This function allocates a new instance of this crypto engine.
 * The key_len parameter should be one of 28 or 44 for
 * AES-128-GCM or AES-256-GCM respectively.  Note that the
 * key length includes the 14 byte salt value that is used when
 * initializing the KDF.
 */
static srtp_err_status_t srtp_aes_gcm_mbedtls_alloc(srtp_cipher_t **c,
                                                    int key_len,
                                                    int tlen)
{
    FUNC_ENTRY();
    srtp_aes_gcm_ctx_t *gcm;

    debug_print(srtp_mod_aes_gcm, "allocating cipher with key length %d",
                key_len);
    debug_print(srtp_mod_aes_gcm, "allocating cipher with tag length %d", tlen);

    /*
     * Verify the key_len is valid for one of: AES-128/256
     */
    if (key_len != SRTP_AES_GCM_128_KEY_LEN_WSALT &&
        key_len != SRTP_AES_GCM_256_KEY_LEN_WSALT) {
        return (srtp_err_status_bad_param);
    }

    if (tlen != GCM_AUTH_TAG_LEN && tlen != GCM_AUTH_TAG_LEN_8) {
        return (srtp_err_status_bad_param);
    }

    /* allocate memory a cipher of type aes_gcm */
    *c = (srtp_cipher_t *)srtp_crypto_alloc(sizeof(srtp_cipher_t));
    if (*c == NULL) {
        return (srtp_err_status_alloc_fail);
    }

    gcm = (srtp_aes_gcm_ctx_t *)srtp_crypto_alloc(sizeof(srtp_aes_gcm_ctx_t));
    if (gcm == NULL) {
        srtp_crypto_free(*c);
        *c = NULL;
        return (srtp_err_status_alloc_fail);
    }

#if MBEDTLS_VERSION_MAJOR >= 4
    gcm->ctx =
        (psa_aes_gcm_ctx_t *)srtp_crypto_alloc(sizeof(psa_aes_gcm_ctx_t));
    if (gcm->ctx == NULL) {
        srtp_crypto_free(gcm);
        srtp_crypto_free(*c);
        *c = NULL;
        return srtp_err_status_alloc_fail;
    }
    gcm->ctx->key_id = PSA_KEY_ID_NULL;
    gcm->ctx->op = psa_aead_operation_init();
#else
    gcm->ctx =
        (mbedtls_gcm_context *)srtp_crypto_alloc(sizeof(mbedtls_gcm_context));
    if (gcm->ctx == NULL) {
        srtp_crypto_free(gcm);
        srtp_crypto_free(*c);
        *c = NULL;
        return srtp_err_status_alloc_fail;
    }
    mbedtls_gcm_init(gcm->ctx);
#endif

    /* set pointers */
    (*c)->state = gcm;

    /* setup cipher attributes */
    switch (key_len) {
    case SRTP_AES_GCM_128_KEY_LEN_WSALT:
        (*c)->type = &srtp_aes_gcm_128;
        (*c)->algorithm = SRTP_AES_GCM_128;
        gcm->key_size = SRTP_AES_128_KEY_LEN;
        gcm->tag_len = tlen;
        break;
    case SRTP_AES_GCM_256_KEY_LEN_WSALT:
        (*c)->type = &srtp_aes_gcm_256;
        (*c)->algorithm = SRTP_AES_GCM_256;
        gcm->key_size = SRTP_AES_256_KEY_LEN;
        gcm->tag_len = tlen;
        break;
    }

    /* set key size        */
    (*c)->key_len = key_len;

    return (srtp_err_status_ok);
}

/*
 * This function deallocates a GCM session
 */
static srtp_err_status_t srtp_aes_gcm_mbedtls_dealloc(srtp_cipher_t *c)
{
    srtp_aes_gcm_ctx_t *ctx;
    FUNC_ENTRY();
    ctx = (srtp_aes_gcm_ctx_t *)c->state;
    if (ctx) {
#if MBEDTLS_VERSION_MAJOR >= 4
        psa_aead_abort(&ctx->ctx->op);
        psa_destroy_key(ctx->ctx->key_id);
#else
        mbedtls_gcm_free(ctx->ctx);
#endif
        srtp_crypto_free(ctx->ctx);
        /* zeroize the key material */
        octet_string_set_to_zero(ctx, sizeof(srtp_aes_gcm_ctx_t));
        srtp_crypto_free(ctx);
    }

    /* free memory */
    srtp_crypto_free(c);

    return (srtp_err_status_ok);
}

static srtp_err_status_t srtp_aes_gcm_mbedtls_context_init(void *cv,
                                                           const uint8_t *key)
{
    FUNC_ENTRY();
    srtp_aes_gcm_ctx_t *c = (srtp_aes_gcm_ctx_t *)cv;
    uint32_t key_len_in_bits;
#if MBEDTLS_VERSION_MAJOR >= 4
    psa_status_t status = PSA_SUCCESS;
#else
    int errCode = 0;
#endif
    c->dir = srtp_direction_any;
    c->aad_size = 0;

    debug_print(srtp_mod_aes_gcm, "key:  %s",
                srtp_octet_string_hex_string(key, c->key_size));
    key_len_in_bits = (c->key_size << 3);
    switch (c->key_size) {
    case SRTP_AES_256_KEY_LEN:
    case SRTP_AES_128_KEY_LEN:
        break;
    default:
        return (srtp_err_status_bad_param);
        break;
    }

#if MBEDTLS_VERSION_MAJOR >= 4
    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        debug_print(srtp_mod_aes_gcm, "psa_crypto_init failed: %d", status);
        return srtp_err_status_init_fail;
    }

    {
        psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_usage_flags(&attr,
                                PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
        psa_set_key_algorithm(
            &attr, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_GCM, c->tag_len));
        psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&attr, key_len_in_bits);

        if (c->ctx->key_id != PSA_KEY_ID_NULL) {
            psa_destroy_key(c->ctx->key_id);
            c->ctx->key_id = PSA_KEY_ID_NULL;
        }

        status =
            psa_import_key(&attr, key, key_len_in_bits / 8, &c->ctx->key_id);
        /* Done with the attributes on both success and failure paths. */
        psa_reset_key_attributes(&attr);
        if (status != PSA_SUCCESS) {
            debug_print(srtp_mod_aes_gcm, "psa_import_key failed: %d", status);
            return srtp_err_status_init_fail;
        }
    }
#else
    errCode = mbedtls_gcm_setkey(c->ctx, MBEDTLS_CIPHER_ID_AES,
                                 (const unsigned char *)key, key_len_in_bits);
    if (errCode != 0) {
        debug_print(srtp_mod_aes_gcm, "mbedtls error code:  %d", errCode);
        return srtp_err_status_init_fail;
    }
#endif

    return (srtp_err_status_ok);
}

static srtp_err_status_t srtp_aes_gcm_mbedtls_set_iv(
    void *cv,
    uint8_t *iv,
    srtp_cipher_direction_t direction)
{
    FUNC_ENTRY();
    srtp_aes_gcm_ctx_t *c = (srtp_aes_gcm_ctx_t *)cv;

    if (direction != srtp_direction_encrypt &&
        direction != srtp_direction_decrypt) {
        return (srtp_err_status_bad_param);
    }
    c->dir = direction;

    /* set_iv marks the start of a fresh packet. Reset accumulated AAD so
     * a prior packet's set_aad() bytes do not bleed into this packet's
     * auth tag (legitimate caller sequence: set_iv -> set_aad -> drop ->
     * set_iv -> set_aad -> encrypt; without this reset the second
     * packet's tag covers the first packet's AAD too). */
    c->aad_size = 0;

    debug_print(srtp_mod_aes_gcm, "setting iv: %s",
                srtp_octet_string_hex_string(iv, GCM_IV_LEN));
    c->iv_len = GCM_IV_LEN;
    memcpy(c->iv, iv, c->iv_len);
    return (srtp_err_status_ok);
}

/*
 * This function processes the AAD
 *
 * Parameters:
 *	c	Crypto context
 *	aad	Additional data to process for AEAD cipher suites
 *	aad_len	length of aad buffer
 */
static srtp_err_status_t srtp_aes_gcm_mbedtls_set_aad(void *cv,
                                                      const uint8_t *aad,
                                                      uint32_t aad_len)
{
    FUNC_ENTRY();
    srtp_aes_gcm_ctx_t *c = (srtp_aes_gcm_ctx_t *)cv;

    debug_print(srtp_mod_aes_gcm, "setting AAD: %s",
                srtp_octet_string_hex_string(aad, aad_len));

    if (aad_len + c->aad_size > MAX_AD_SIZE) {
        return srtp_err_status_bad_param;
    }

    memcpy(c->aad + c->aad_size, aad, aad_len);
    c->aad_size += aad_len;

    return (srtp_err_status_ok);
}

/*
 * This function encrypts a buffer using AES GCM mode
 *
 * Parameters:
 *	c	Crypto context
 *	buf	data to encrypt
 *	enc_len	length of encrypt buffer
 */
static srtp_err_status_t srtp_aes_gcm_mbedtls_encrypt(void *cv,
                                                      unsigned char *buf,
                                                      unsigned int *enc_len)
{
    FUNC_ENTRY();
    srtp_aes_gcm_ctx_t *c = (srtp_aes_gcm_ctx_t *)cv;

    /* Require dir == encrypt to catch cross-call bugs (caller passing a
     * decrypt-configured ctx into the encrypt entry point). PSA does not
     * catch this on its own since the imported key has both ENCRYPT and
     * DECRYPT usage flags. */
    if (c->dir != srtp_direction_encrypt) {
        return (srtp_err_status_bad_param);
    }

#if MBEDTLS_VERSION_MAJOR >= 4
    {
        /*
         * libsrtp 2.x AES-GCM encrypt semantics:
         *   - ciphertext is written in-place into `buf` (no tag appended)
         *   - the auth tag is stashed in c->tag and later returned via
         * get_tag() PSA's psa_aead_encrypt writes ciphertext+tag concatenated,
         * so we use the multipart API which exposes the tag as a separate
         * output, allowing us to keep the in-place ciphertext layout the 2.x
         * callers expect.
         */
        psa_status_t status;
        psa_aead_operation_t op = psa_aead_operation_init();
        size_t out_off = 0;
        size_t out_len = 0;
        size_t tag_out_len = 0;

        status = psa_aead_encrypt_setup(
            &op, c->ctx->key_id,
            PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_GCM, c->tag_len));
        if (status != PSA_SUCCESS) {
            debug_print(srtp_mod_aes_gcm, "psa_aead_encrypt_setup failed: %d",
                        status);
            goto enc_fail;
        }

        status = psa_aead_set_lengths(&op, c->aad_size, *enc_len);
        if (status != PSA_SUCCESS) {
            debug_print(srtp_mod_aes_gcm, "psa_aead_set_lengths failed: %d",
                        status);
            goto enc_fail;
        }

        status = psa_aead_set_nonce(&op, c->iv, c->iv_len);
        if (status != PSA_SUCCESS) {
            debug_print(srtp_mod_aes_gcm, "psa_aead_set_nonce failed: %d",
                        status);
            goto enc_fail;
        }

        if (c->aad_size > 0) {
            status = psa_aead_update_ad(&op, c->aad, c->aad_size);
            if (status != PSA_SUCCESS) {
                debug_print(srtp_mod_aes_gcm, "psa_aead_update_ad failed: %d",
                            status);
                goto enc_fail;
            }
        }

        /*
         * psa_aead_update output buffer must be at least
         * PSA_AEAD_UPDATE_OUTPUT_SIZE bytes (block-aligned for GCM); pass the
         * full payload length which is always sufficient. In-place src==dst is
         * permitted for PSA AEAD.
         *
         * AAD-only authentication is legal: *enc_len == 0 and buf may be
         * NULL. Some PSA backends reject a NULL output pointer even when
         * output_size is 0, so guard symmetrically with the decrypt path.
         */
        out_len = 0;
        if (*enc_len > 0) {
            status =
                psa_aead_update(&op, buf, *enc_len, buf, *enc_len, &out_len);
            if (status != PSA_SUCCESS) {
                debug_print(srtp_mod_aes_gcm, "psa_aead_update failed: %d",
                            status);
                goto enc_fail;
            }
        }
        out_off = out_len;

        /*
         * psa_aead_finish writes any trailing ciphertext to a separate buffer.
         * For GCM the trailing part is always 0 bytes, but the spec allows the
         * implementation to defer up to one block, so we route it through a
         * 16-byte scratch and copy any returned bytes back into buf at out_off.
         */
        {
            uint8_t finish_scratch[16];
            status =
                psa_aead_finish(&op, finish_scratch, sizeof(finish_scratch),
                                &out_len, c->tag, sizeof(c->tag), &tag_out_len);
            if (status != PSA_SUCCESS) {
                debug_print(srtp_mod_aes_gcm, "psa_aead_finish failed: %d",
                            status);
                goto enc_fail;
            }
            if (out_len > 0) {
                if (out_off + out_len > *enc_len) {
                    psa_aead_abort(&op);
                    c->aad_size = 0;
                    return srtp_err_status_cipher_fail;
                }
                memcpy(buf + out_off, finish_scratch, out_len);
            }
        }

        if (tag_out_len != (size_t)c->tag_len) {
            psa_aead_abort(&op);
            c->aad_size = 0;
            return srtp_err_status_cipher_fail;
        }

        c->aad_size = 0;
        return srtp_err_status_ok;

    enc_fail:
        psa_aead_abort(&op);
        c->aad_size = 0;
        return srtp_err_status_cipher_fail;
    }
#else
    {
        int errCode = mbedtls_gcm_crypt_and_tag(
            c->ctx, MBEDTLS_GCM_ENCRYPT, *enc_len, c->iv, c->iv_len, c->aad,
            c->aad_size, buf, buf, c->tag_len, c->tag);

        c->aad_size = 0;
        if (errCode != 0) {
            debug_print(srtp_mod_aes_gcm, "mbedtls error code:  %d", errCode);
            return srtp_err_status_bad_param;
        }

        return (srtp_err_status_ok);
    }
#endif
}

/*
 * This function calculates and returns the GCM tag for a given context.
 * This should be called after encrypting the data.  The *len value
 * is increased by the tag size.  The caller must ensure that *buf has
 * enough room to accept the appended tag.
 *
 * Parameters:
 *	c	Crypto context
 *	buf	data to encrypt
 *	len	length of encrypt buffer
 */
static srtp_err_status_t srtp_aes_gcm_mbedtls_get_tag(void *cv,
                                                      uint8_t *buf,
                                                      uint32_t *len)
{
    FUNC_ENTRY();
    srtp_aes_gcm_ctx_t *c = (srtp_aes_gcm_ctx_t *)cv;
    debug_print(srtp_mod_aes_gcm, "appended tag size:  %d", c->tag_len);
    *len = c->tag_len;
    memcpy(buf, c->tag, c->tag_len);
    return (srtp_err_status_ok);
}

/*
 * This function decrypts a buffer using AES GCM mode
 *
 * Parameters:
 *	c	Crypto context
 *	buf	data to encrypt
 *	enc_len	length of encrypt buffer
 */
static srtp_err_status_t srtp_aes_gcm_mbedtls_decrypt(void *cv,
                                                      unsigned char *buf,
                                                      unsigned int *enc_len)
{
    FUNC_ENTRY();
    srtp_aes_gcm_ctx_t *c = (srtp_aes_gcm_ctx_t *)cv;

    /* Require dir == decrypt to catch cross-call bugs (see encrypt for
     * rationale). */
    if (c->dir != srtp_direction_decrypt) {
        return (srtp_err_status_bad_param);
    }

    debug_print(srtp_mod_aes_gcm, "AAD: %s",
                srtp_octet_string_hex_string(c->aad, c->aad_size));

#if MBEDTLS_VERSION_MAJOR >= 4
    {
        /*
         * libsrtp 2.x AES-GCM decrypt semantics:
         *   - input buf holds ciphertext || tag of total length *enc_len
         *   - plaintext is written in-place starting at buf
         *   - on success *enc_len is reduced by c->tag_len
         *
         * PSA exposes the tag as a separate input to psa_aead_verify, so we
         * use the multipart API and feed buf[..ct_len] to update and the
         * trailing c->tag_len bytes to verify.
         */
        psa_status_t status;
        psa_aead_operation_t op = psa_aead_operation_init();
        size_t ct_len;
        size_t out_off = 0;
        size_t out_len = 0;

        if ((unsigned int)c->tag_len > *enc_len) {
            c->aad_size = 0;
            return srtp_err_status_bad_param;
        }
        ct_len = (size_t)(*enc_len - (unsigned int)c->tag_len);

        status = psa_aead_decrypt_setup(
            &op, c->ctx->key_id,
            PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_GCM, c->tag_len));
        if (status != PSA_SUCCESS) {
            debug_print(srtp_mod_aes_gcm, "psa_aead_decrypt_setup failed: %d",
                        status);
            goto dec_fail;
        }

        status = psa_aead_set_lengths(&op, c->aad_size, ct_len);
        if (status != PSA_SUCCESS) {
            debug_print(srtp_mod_aes_gcm, "psa_aead_set_lengths failed: %d",
                        status);
            goto dec_fail;
        }

        status = psa_aead_set_nonce(&op, c->iv, c->iv_len);
        if (status != PSA_SUCCESS) {
            debug_print(srtp_mod_aes_gcm, "psa_aead_set_nonce failed: %d",
                        status);
            goto dec_fail;
        }

        if (c->aad_size > 0) {
            status = psa_aead_update_ad(&op, c->aad, c->aad_size);
            if (status != PSA_SUCCESS) {
                debug_print(srtp_mod_aes_gcm, "psa_aead_update_ad failed: %d",
                            status);
                goto dec_fail;
            }
        }

        /* In-place src==dst is permitted for PSA AEAD. */
        if (ct_len > 0) {
            status = psa_aead_update(&op, buf, ct_len, buf, ct_len, &out_len);
            if (status != PSA_SUCCESS) {
                debug_print(srtp_mod_aes_gcm, "psa_aead_update failed: %d",
                            status);
                goto dec_fail;
            }
            out_off = out_len;
        }

        {
            uint8_t finish_scratch[16];
            status =
                psa_aead_verify(&op, finish_scratch, sizeof(finish_scratch),
                                &out_len, buf + ct_len, (size_t)c->tag_len);
            if (status != PSA_SUCCESS) {
                psa_aead_abort(&op);
                c->aad_size = 0;
                return srtp_err_status_auth_fail;
            }
            if (out_len > 0) {
                if (out_off + out_len > ct_len) {
                    psa_aead_abort(&op);
                    c->aad_size = 0;
                    return srtp_err_status_cipher_fail;
                }
                memcpy(buf + out_off, finish_scratch, out_len);
            }
        }

        c->aad_size = 0;
        *enc_len -= c->tag_len;
        return srtp_err_status_ok;

    dec_fail:
        psa_aead_abort(&op);
        c->aad_size = 0;
        return srtp_err_status_auth_fail;
    }
#else
    {
        int errCode = mbedtls_gcm_auth_decrypt(
            c->ctx, (*enc_len - c->tag_len), c->iv, c->iv_len, c->aad,
            c->aad_size, buf + (*enc_len - c->tag_len), c->tag_len, buf, buf);
        c->aad_size = 0;
        if (errCode != 0) {
            return (srtp_err_status_auth_fail);
        }

        /*
         * Reduce the buffer size by the tag length since the tag
         * is not part of the original payload
         */
        *enc_len -= c->tag_len;

        return (srtp_err_status_ok);
    }
#endif
}

/*
 * Name of this crypto engine
 */
static const char srtp_aes_gcm_128_mbedtls_description[] =
    "AES-128 GCM using mbedtls";
static const char srtp_aes_gcm_256_mbedtls_description[] =
    "AES-256 GCM using mbedtls";

/*
 * This is the vector function table for this crypto engine.
 */
const srtp_cipher_type_t srtp_aes_gcm_128 = {
    srtp_aes_gcm_mbedtls_alloc,
    srtp_aes_gcm_mbedtls_dealloc,
    srtp_aes_gcm_mbedtls_context_init,
    srtp_aes_gcm_mbedtls_set_aad,
    srtp_aes_gcm_mbedtls_encrypt,
    srtp_aes_gcm_mbedtls_decrypt,
    srtp_aes_gcm_mbedtls_set_iv,
    srtp_aes_gcm_mbedtls_get_tag,
    srtp_aes_gcm_128_mbedtls_description,
    &srtp_aes_gcm_128_test_case_0,
    SRTP_AES_GCM_128
};

/*
 * This is the vector function table for this crypto engine.
 */
const srtp_cipher_type_t srtp_aes_gcm_256 = {
    srtp_aes_gcm_mbedtls_alloc,
    srtp_aes_gcm_mbedtls_dealloc,
    srtp_aes_gcm_mbedtls_context_init,
    srtp_aes_gcm_mbedtls_set_aad,
    srtp_aes_gcm_mbedtls_encrypt,
    srtp_aes_gcm_mbedtls_decrypt,
    srtp_aes_gcm_mbedtls_set_iv,
    srtp_aes_gcm_mbedtls_get_tag,
    srtp_aes_gcm_256_mbedtls_description,
    &srtp_aes_gcm_256_test_case_0,
    SRTP_AES_GCM_256
};
