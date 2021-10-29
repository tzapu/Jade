#include "../button_events.h"
#include "../jade_assert.h"
#include "../jade_wally_verify.h"
#include "../keychain.h"
#include "../process.h"
#include "../sensitive.h"
#include "../ui.h"
#include "../utils/cbor_rpc.h"
#include "../utils/event.h"
#include "../utils/malloc_ext.h"
#include "../utils/network.h"
#include "../wallet.h"

#include <mbedtls/sha256.h>
#include <sodium/utils.h>
#include <wally_elements.h>

#include "process_utils.h"

// From sign_tx.c
script_flavour_t get_script_flavour(const uint8_t* script, size_t script_len);
void update_aggregate_scripts_flavour(script_flavour_t new_script_flavour, script_flavour_t* aggregate_scripts_flavour);
bool validate_change_paths(jade_process_t* process, const char* network, const struct wally_tx* tx, CborValue* change,
    output_info_t* output_info, const char** errmsg);
void send_ae_signature_replies(jade_process_t* process, signing_data_t* all_signing_data, uint32_t num_inputs);
void send_ec_signature_replies(jade_msg_source_t source, signing_data_t* all_signing_data, uint32_t num_inputs);

static void wally_free_tx_wrapper(void* tx) { JADE_WALLY_VERIFY(wally_tx_free((struct wally_tx*)tx)); }

static inline void value_to_le(const uint32_t val, unsigned char* buffer)
{
    buffer[0] = val & 0xFF;
    buffer[1] = (val >> 8) & 0xFF;
    buffer[2] = (val >> 16) & 0xFF;
    buffer[3] = (val >> 24) & 0xFF;
}

static bool add_confidential_output_info(const commitment_t* commitments, const struct wally_tx_output* txoutput,
    output_info_t* outinfo, const char** errmsg)
{
    JADE_ASSERT(txoutput);
    JADE_ASSERT(outinfo);
    JADE_ASSERT(errmsg);
    JADE_ASSERT(txoutput->value[0] != 0x01); // Don't call for unblinded outputs
    JADE_ASSERT(commitments);

    if (!commitments->have_commitments) {
        *errmsg = "Missing commitments data for blinded output";
        return false;
    }

    // 1. Copy the 'trusted' commitments into the tx so we sign over them
    if (txoutput->asset_len != sizeof(commitments->asset_generator)) {
        *errmsg = "Failed to update tx asset_generator from commitments data";
        return false;
    }
    memcpy(txoutput->asset, commitments->asset_generator, sizeof(commitments->asset_generator));

    if (txoutput->value_len != sizeof(commitments->value_commitment)) {
        *errmsg = "Failed to update tx value_commitment from commitments data";
        return false;
    }
    memcpy(txoutput->value, commitments->value_commitment, sizeof(commitments->value_commitment));

    // 2. Fetch the asset_id, value, and blinding_key into the info struct
    JADE_ASSERT(sizeof(outinfo->asset_id) == sizeof(commitments->asset_id));
    JADE_ASSERT(sizeof(outinfo->blinding_key) == sizeof(commitments->blinding_key));
    memcpy(outinfo->asset_id, commitments->asset_id, sizeof(commitments->asset_id));
    outinfo->value = commitments->value;
    memcpy(outinfo->blinding_key, commitments->blinding_key, sizeof(commitments->blinding_key));

    return true;
}

static bool check_trusted_commitment_valid(const unsigned char* hash_prevouts, const size_t hash_prevouts_len,
    const int idx, const struct wally_tx_output* txoutput, const commitment_t* commitments, bool* found_odd_vbf,
    const char** errmsg)
{
    JADE_ASSERT(hash_prevouts);
    JADE_ASSERT(hash_prevouts_len == SHA256_LEN);
    JADE_ASSERT(idx >= 0);
    JADE_ASSERT(txoutput);
    JADE_ASSERT(found_odd_vbf);
    JADE_ASSERT(errmsg);
    JADE_ASSERT(txoutput->value[0] != 0x01); // Don't call for unblinded outputs
    JADE_ASSERT(commitments);

    unsigned char bf_tmp_buffer[HMAC_SHA256_LEN];
    unsigned char generator_tmp[ASSET_GENERATOR_LEN];
    unsigned char commitment_tmp[ASSET_COMMITMENT_LEN];

    // Check the abf. if the host lied about hash_prevouts in get_blinding_factor/get_commitments we will detect it here
    // ALL abfs MUST be correct.
    if (!wallet_get_blinding_factor(
            hash_prevouts, hash_prevouts_len, idx, ASSET_BLINDING_FACTOR, bf_tmp_buffer, sizeof(bf_tmp_buffer))
        || wally_asset_generator_from_bytes(commitments->asset_id, sizeof(commitments->asset_id), bf_tmp_buffer,
               sizeof(bf_tmp_buffer), generator_tmp, sizeof(generator_tmp))
            != WALLY_OK
        || sodium_memcmp(commitments->asset_generator, generator_tmp, ASSET_GENERATOR_LEN) != 0
        || sodium_memcmp(txoutput->asset, generator_tmp, ASSET_GENERATOR_LEN) != 0) {
        *errmsg = "Failed to verify asset_generator from commitments data";
        return false;
    }

    // check the vbf.
    if (!wallet_get_blinding_factor(
            hash_prevouts, hash_prevouts_len, idx, VALUE_BLINDING_FACTOR, bf_tmp_buffer, sizeof(bf_tmp_buffer))
        || wally_asset_value_commitment(commitments->value, bf_tmp_buffer, sizeof(bf_tmp_buffer), generator_tmp,
               sizeof(generator_tmp), commitment_tmp, sizeof(commitment_tmp))
            != WALLY_OK) {
        *errmsg = "Failed to verify value_commitment from commitments data";
        return false;
    }

    // here we allow AT MOST one vbf/value-commitment to be "unexpected"
    if (sodium_memcmp(commitments->value_commitment, commitment_tmp, ASSET_COMMITMENT_LEN) != 0
        || sodium_memcmp(txoutput->value, commitment_tmp, ASSET_COMMITMENT_LEN) != 0) {
        JADE_LOGI("Found mismatching vbf/value_commitment at index %u (one is expected per tx)", idx);
        if (!(*found_odd_vbf)) {
            // Record seeing odd vbf
            *found_odd_vbf = true;
        } else {
            // Error on subsequent
            *errmsg = "Failed to verify value_commitment from commitments data";
            return false;
        }
    }

    // re-compute and check hmac of the provided trusted commitment
    unsigned char signed_blob[ASSET_GENERATOR_LEN + ASSET_COMMITMENT_LEN + ASSET_TAG_LEN + sizeof(uint64_t)];
    unsigned char* p = signed_blob;
    memcpy(p, commitments->asset_generator, ASSET_GENERATOR_LEN);
    p += ASSET_GENERATOR_LEN;
    memcpy(p, commitments->value_commitment, ASSET_COMMITMENT_LEN);
    p += ASSET_COMMITMENT_LEN;
    memcpy(p, commitments->asset_id, ASSET_TAG_LEN);
    p += ASSET_TAG_LEN;
    memcpy(p, &commitments->value, sizeof(uint64_t));

    unsigned char our_hmac[HMAC_SHA256_LEN];
    if (!wallet_hmac_with_master_key(signed_blob, sizeof(signed_blob), our_hmac, sizeof(our_hmac))
        || sodium_memcmp(our_hmac, commitments->hmac, HMAC_SHA256_LEN) != 0) {
        *errmsg = "Failed to verify hmac from commitments data";
        return false;
    }

    // All good
    return true;
}

/*
 * The message flow here is complicated because we cater for both a legacy flow
 * for standard deterministic EC signatures (see rfc6979) and a newer message
 * exchange added later to cater for anti-exfil signatures.
 * At the moment we retain the older message flow for backward compatibility,
 * but at some point we should remove it and use the new message flow for all
 * cases, which would simplify the code here and in the client.
 */
void sign_liquid_tx_process(void* process_ptr)
{
    JADE_LOGI("Starting: %u", xPortGetFreeHeapSize());
    jade_process_t* process = process_ptr;
    char network[MAX_NETWORK_NAME_LEN];

    // Context used to compute hash_prevout (hash of all the input scriptpubkeys)
    mbedtls_sha256_context hash_prevout_sha_ctx;
    mbedtls_sha256_init(&hash_prevout_sha_ctx);

    // We expect a current message to be present
    ASSERT_CURRENT_MESSAGE(process, "sign_liquid_tx");
    GET_MSG_PARAMS(process);
    const jade_msg_source_t source = process->ctx.source;

    // Check network is valid and consistent with prior usage
    size_t written = 0;
    rpc_get_string("network", sizeof(network), &params, network, &written);
    CHECK_NETWORK_CONSISTENT(process, network, written);
    if (!isLiquidNetwork(network)) {
        jade_process_reject_message(
            process, CBOR_RPC_BAD_PARAMETERS, "sign_liquid_tx call only appropriate for liquid network", NULL);
        goto cleanup;
    }

    written = 0;
    const uint8_t* txbytes = NULL;
    rpc_get_bytes_ptr("txn", &params, &txbytes, &written);

    if (written == 0) {
        jade_process_reject_message(process, CBOR_RPC_BAD_PARAMETERS, "Failed to extract txn from parameters", NULL);
        goto cleanup;
    }
    JADE_ASSERT(txbytes);

    struct wally_tx* tx = NULL;
    int res = wally_tx_from_bytes(txbytes, written, WALLY_TX_FLAG_USE_ELEMENTS, &tx); // elements, without witness
    if (res != WALLY_OK || !tx) {
        jade_process_reject_message(process, CBOR_RPC_BAD_PARAMETERS, "Failed to extract tx from passed bytes", NULL);
        goto cleanup;
    }
    jade_process_call_on_exit(process, wally_free_tx_wrapper, tx);

    // copy the amount
    uint32_t num_inputs = 0;
    bool retval = rpc_get_sizet("num_inputs", &params, &num_inputs);
    if (!retval || num_inputs == 0) {
        jade_process_reject_message(
            process, CBOR_RPC_BAD_PARAMETERS, "Failed to extract valid number of inputs from parameters", NULL);
        goto cleanup;
    }

    // Check the number of inputs the client wants to send is what we
    // would expect for the given transaction.  Fail if not.
    if (num_inputs != tx->num_inputs) {
        jade_process_reject_message(
            process, CBOR_RPC_BAD_PARAMETERS, "Unexpected number of inputs for transaction", NULL);
        goto cleanup;
    }

    // Copy trusted commitment data into a temporary structure (so we can free the message)
    commitment_t* commitments = NULL;
    size_t num_commitments = 0;
    rpc_get_commitments_allocate("trusted_commitments", &params, &commitments, &num_commitments);

    if (num_commitments == 0) {
        jade_process_reject_message(
            process, CBOR_RPC_BAD_PARAMETERS, "Failed to extract trusted commitments from parameters", NULL);
        goto cleanup;
    }

    JADE_ASSERT(commitments);
    jade_process_free_on_exit(process, commitments);

    // Check the trusted commitments: expect one element in the array for each output.
    // (Can be null/zero's for unblinded outputs.)
    if (num_commitments != tx->num_outputs) {
        jade_process_reject_message(
            process, CBOR_RPC_BAD_PARAMETERS, "Unexpected number of trusted commitments for transaction", NULL);
        goto cleanup;
    }

    // We always need this extra data to 'unblind' confidential txns
    output_info_t* output_info = JADE_CALLOC(tx->num_outputs, sizeof(output_info_t));
    jade_process_free_on_exit(process, output_info);

    // Whether to use Anti-Exfil signatures and message flow
    // Optional flag, defaults to false
    bool use_ae_signatures = false;
    rpc_get_boolean("use_ae_signatures", &params, &use_ae_signatures);

    // Can optionally be passed paths for change outputs, which we verify internally
    const char* errmsg = NULL;
    CborValue change;
    if (rpc_get_array("change", &params, &change)) {
        if (!validate_change_paths(process, network, tx, &change, output_info, &errmsg)) {
            jade_process_reject_message(process, CBOR_RPC_BAD_PARAMETERS, errmsg, NULL);
            goto cleanup;
        }
    }

    // Prepare a hash to compute hash_prevout (hash of all the input scriptpubkeys)
    res = mbedtls_sha256_starts_ret(&hash_prevout_sha_ctx, 0); // 0 = SHA256 instead of SHA224
    if (res != 0) {
        jade_process_reject_message(process, CBOR_RPC_INTERNAL_ERROR, "Failed to initialise prevout hash", NULL);
        goto cleanup;
    }

    // save fees for the final confirmation screen
    uint64_t fees = 0;

    // populate an `output_index` -> (blinding_key, asset, value) map
    for (size_t i = 0; i < tx->num_outputs; ++i) {
        if (tx->outputs[i].value[0] == 0x01) {
            // unconfidential, take directly from the tx
            output_info[i].is_confidential = false;

            memcpy(output_info[i].asset_id, tx->outputs[i].asset + 1, 32);
            wally_tx_confidential_value_to_satoshi(
                tx->outputs[i].value, tx->outputs[i].value_len, &output_info[i].value);

            // fees can only be unconfidential
            if (!tx->outputs[i].script) {
                fees += output_info[i].value;
            }
        } else {
            // confidential, use the trusted_commitments
            output_info[i].is_confidential = true;

            const char* errmsg = NULL;
            if (!add_confidential_output_info(&(commitments[i]), &(tx->outputs[i]), &output_info[i], &errmsg)) {
                jade_process_reject_message(process, CBOR_RPC_BAD_PARAMETERS, errmsg, NULL);
                goto cleanup;
            }
        }
    }

    gui_activity_t* first_activity = NULL;
    make_display_elements_output_activity(network, tx, output_info, &first_activity);
    JADE_ASSERT(first_activity);
    gui_set_current_activity(first_activity);

    // ----------------------------------
    // wait for the last "next" (proceed with the protocol and then final confirmation)
    int32_t ev_id;
    // In a debug unattended ci build, assume buttons pressed after a short delay
#ifndef CONFIG_DEBUG_UNATTENDED_CI
    const esp_err_t outputs_ret = sync_await_single_event(JADE_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);
#else
    vTaskDelay(CONFIG_DEBUG_UNATTENDED_CI_TIMEOUT_MS / portTICK_PERIOD_MS);
    const esp_err_t outputs_ret = ESP_OK;
    ev_id = SIGN_TX_ACCEPT_OUTPUTS;
#endif

    // Check to see whether user accepted or declined
    if (outputs_ret != ESP_OK || ev_id != SIGN_TX_ACCEPT_OUTPUTS) {
        JADE_LOGW("User declined to sign transaction");
        jade_process_reject_message(process, CBOR_RPC_USER_CANCELLED, "User declined to sign transaction", NULL);
        goto cleanup;
    }

    JADE_LOGD("User accepted outputs");
    display_message_activity("Processing...");

    // Send ok - client should send inputs
    jade_process_reply_to_message_ok(process);

    // We generate the hashes for each input but defer signing them
    // until after the final user confirmation.  Hold them in an block for
    // ease of cleanup if something goes wrong part-way through.
    signing_data_t* const all_signing_data = JADE_CALLOC(num_inputs, sizeof(signing_data_t));
    jade_process_free_on_exit(process, all_signing_data);

    // We track if the type of the inputs we are signing changes (ie. single-sig vs
    // green/multisig/other) so we can show a warning to the user if so.
    script_flavour_t aggregate_inputs_scripts_flavour = SCRIPT_FLAVOUR_NONE;

    // Run through each input message and generate a signature for each one
    for (size_t index = 0; index < num_inputs; ++index) {
        jade_process_load_in_message(process, true);
        if (!IS_CURRENT_MESSAGE(process, "tx_input")) {
            // Protocol error
            jade_process_reject_message(
                process, CBOR_RPC_PROTOCOL_ERROR, "Unexpected message, expecting 'tx_input'", NULL);
            goto cleanup;
        }

        // txn input as expected - get input parameters
        GET_MSG_PARAMS(process);

        size_t script_len = 0;
        const uint8_t* script = NULL;

        // The ae commitments for this input (if using anti-exfil signatures)
        size_t ae_host_commitment_len = 0;
        const uint8_t* ae_host_commitment = NULL;
        uint8_t ae_signer_commitment[WALLY_S2C_OPENING_LEN];

        // Make and store the reply data, and then delete the (potentially
        // large) input message.  Replies will be sent after user confirmation.
        written = 0;
        signing_data_t* const sig_data = all_signing_data + index;
        rpc_get_id(&process->ctx.value, sig_data->id, sizeof(sig_data->id), &written);
        JADE_ASSERT(written != 0);

        bool is_witness = false;
        retval = rpc_get_boolean("is_witness", &params, &is_witness);
        if (!retval) {
            jade_process_reject_message(
                process, CBOR_RPC_BAD_PARAMETERS, "Failed to extract is_witness from parameters", NULL);
            goto cleanup;
        }

        // Path node can be omitted if we don't want to sign this input
        // (But if passed must be valid - empty/root path is not allowed for signing)
        const bool has_path = rpc_has_field_data("path", &params);
        if (has_path) {
            const size_t max_path_len = sizeof(sig_data->path) / sizeof(sig_data->path[0]);
            if (!rpc_get_bip32_path("path", &params, sig_data->path, max_path_len, &sig_data->path_len)
                || sig_data->path_len == 0) {
                jade_process_reject_message(
                    process, CBOR_RPC_BAD_PARAMETERS, "Failed to extract valid path from parameters", NULL);
                goto cleanup;
            }

            // If required, read anti-exfil host commitment data
            if (use_ae_signatures) {
                rpc_get_bytes_ptr("ae_host_commitment", &params, &ae_host_commitment, &ae_host_commitment_len);
                if (!ae_host_commitment || ae_host_commitment_len != WALLY_HOST_COMMITMENT_LEN) {
                    jade_process_reject_message(process, CBOR_RPC_BAD_PARAMETERS,
                        "Failed to extract valid host commitment from parameters", NULL);
                    goto cleanup;
                }
            }

            // Get prevout script - required for signing inputs
            rpc_get_bytes_ptr("script", &params, &script, &script_len);
            if (!script || script_len == 0) {
                jade_process_reject_message(
                    process, CBOR_RPC_BAD_PARAMETERS, "Failed to extract script from parameters", NULL);
                goto cleanup;
            }

            // Track the types of the input prevout scripts
            const script_flavour_t script_flavour = get_script_flavour(script, script_len);
            update_aggregate_scripts_flavour(script_flavour, &aggregate_inputs_scripts_flavour);
        }

        // update hash_prevouts with the current output being spent
        unsigned char index_little_endian[sizeof(uint32_t)] = { 0 };
        value_to_le(tx->inputs[index].index, index_little_endian);

        if (mbedtls_sha256_update_ret(&hash_prevout_sha_ctx, tx->inputs[index].txhash, WALLY_TXHASH_LEN) != 0
            || mbedtls_sha256_update_ret(&hash_prevout_sha_ctx, index_little_endian, sizeof(index_little_endian))
                != 0) {
            jade_process_reject_message(process, CBOR_RPC_INTERNAL_ERROR, "Failed to build prevout hash", NULL);
            goto cleanup;
        }

        uint32_t value_len = 0;
        const uint8_t* value_commitment = NULL;
        if (has_path && is_witness) {
            JADE_LOGD("For segwit input using explicitly passed value_commitment");

            rpc_get_bytes_ptr("value_commitment", &params, &value_commitment, &value_len);
            if (value_len != ASSET_COMMITMENT_LEN) {
                jade_process_reject_message(
                    process, CBOR_RPC_BAD_PARAMETERS, "Failed to extract value commitment from parameters", NULL);
                goto cleanup;
            }
        }

        // Make signature if given a path (should have a script in hand)
        if (has_path) {
            JADE_ASSERT(script);
            JADE_ASSERT(script_len > 0);
            JADE_ASSERT(sig_data->path_len > 0);

            // Generate hash of this input which we will sign later
            if (!wallet_get_elements_tx_input_hash(tx, index, is_witness, script, script_len,
                    value_len == 0 ? NULL : value_commitment, value_len, sig_data->signature_hash,
                    sizeof(sig_data->signature_hash))) {
                jade_process_reject_message(process, CBOR_RPC_INTERNAL_ERROR, "Failed to make tx input hash", NULL);
                goto cleanup;
            }

            // If using anti-exfil signatures, compute signer commitment for returning to caller
            if (use_ae_signatures) {
                JADE_ASSERT(ae_host_commitment);
                JADE_ASSERT(ae_host_commitment_len == WALLY_HOST_COMMITMENT_LEN);
                if (!wallet_get_signer_commitment(sig_data->signature_hash, sizeof(sig_data->signature_hash),
                        sig_data->path, sig_data->path_len, ae_host_commitment, ae_host_commitment_len,
                        ae_signer_commitment, sizeof(ae_signer_commitment))) {
                    jade_process_reject_message(
                        process, CBOR_RPC_INTERNAL_ERROR, "Failed to make ae signer commitment", NULL);
                    goto cleanup;
                }
            }
        } else {
            // Empty byte-string reply (no path given implies no sig needed or expected)
            JADE_ASSERT(!script);
            JADE_ASSERT(script_len == 0);
            JADE_ASSERT(sig_data->path_len == 0);
        }

        // If using ae-signatures, reply with the signer commitment
        // FIXME: change message flow to reply here even when not using ae-signatures
        // as this simplifies the code both here and in the client.
        if (use_ae_signatures) {
            uint8_t buffer[256];
            jade_process_reply_to_message_bytes(process->ctx, ae_signer_commitment,
                has_path ? sizeof(ae_signer_commitment) : 0, buffer, sizeof(buffer));
        }
    }

    // Finalize the hash_prevout hash
    unsigned char hash_prevouts_single[SHA256_LEN];
    res = mbedtls_sha256_finish_ret(&hash_prevout_sha_ctx, hash_prevouts_single);
    if (res != 0) {
        jade_process_reject_message(process, CBOR_RPC_INTERNAL_ERROR, "Failed to compute prevout hash", NULL);
        goto cleanup;
    }

    // BIP143 says to do a double sha
    unsigned char hash_prevouts_double[SHA256_LEN];
    JADE_WALLY_VERIFY(wally_sha256(hash_prevouts_single, SHA256_LEN, hash_prevouts_double, SHA256_LEN));

    // char *hex_prevouts = NULL;
    // wally_hex_from_bytes(hash_prevouts_double, SHA256_LEN, &hex_prevouts);
    // JADE_LOGI("prevouts_hash calculated: %s", hex_str);
    // wally_free_string(hex_str);

    // Check the trusted commitments: expect one element in the array for each output.
    // Can be null for unblinded outputs as will skip them.
    // Allow at most one unexpeced vbf/value_commitment (as one is not the usual
    // randomish value, but is calculated so the commitments add up correctly.)
    bool found_odd_vbf = false;
    for (size_t i = 0; i < tx->num_outputs; ++i) {
        // unblinded prefix, continue
        if (tx->outputs[i].value[0] == 0x01) {
            continue;
        }

        const char* errmsg = NULL;
        if (!check_trusted_commitment_valid(hash_prevouts_double, sizeof(hash_prevouts_double), i, &(tx->outputs[i]),
                &(commitments[i]), &found_odd_vbf, &errmsg)) {
            // If commitment data invalid, we'll send the 'cancelled' error response for the first input message only
            jade_process_reject_message(process, CBOR_RPC_BAD_PARAMETERS, errmsg, NULL);
            goto cleanup;
        }
    }

    gui_activity_t* final_activity;
    const char* const warning_msg
        = aggregate_inputs_scripts_flavour == SCRIPT_FLAVOUR_MIXED ? WARN_MSG_MIXED_INPUTS : NULL;
    make_display_elements_final_confirmation_activity(fees, warning_msg, &final_activity);
    JADE_ASSERT(final_activity);
    gui_set_current_activity(final_activity);

    // ----------------------------------
    // Wait for the confirmation btn
    // In a debug unattended ci build, assume 'accept' button pressed after a short delay
#ifndef CONFIG_DEBUG_UNATTENDED_CI
    const bool fee_ret
        = gui_activity_wait_event(final_activity, GUI_BUTTON_EVENT, ESP_EVENT_ANY_ID, NULL, &ev_id, NULL, 0);
#else
    vTaskDelay(CONFIG_DEBUG_UNATTENDED_CI_TIMEOUT_MS / portTICK_PERIOD_MS);
    const bool fee_ret = true;
    ev_id = BTN_ACCEPT_SIGNATURE;
#endif

    // If user cancels we'll send the 'cancelled' error response for the last input message only
    if (!fee_ret || ev_id != BTN_ACCEPT_SIGNATURE) {
        // If using ae-signatures, we need to load the message to send the error back on
        if (use_ae_signatures) {
            jade_process_load_in_message(process, true);
        }
        JADE_LOGW("User declined to sign transaction");
        jade_process_reject_message(process, CBOR_RPC_USER_CANCELLED, "User declined to sign transaction", NULL);
        goto cleanup;
    }

    JADE_LOGD("User accepted fee");
    display_message_activity("Processing...");

    // Send signature replies.
    // NOTE: currently we have two message flows - the backward compatible version
    // for normal EC signatures, and the new flow required for Anti-Exfil signatures.
    // Once we have migrated the companion applications onto AE signatures we should
    // convert normal EC signatures to use the new/improved message flow.
    if (use_ae_signatures) {
        // Generate and send Anti-Exfil signature replies
        send_ae_signature_replies(process, all_signing_data, num_inputs);
    } else {
        // Generate and send standard EC signature replies
        send_ec_signature_replies(source, all_signing_data, num_inputs);
    }
    JADE_LOGI("Success");

cleanup:
    mbedtls_sha256_free(&hash_prevout_sha_ctx);
}
