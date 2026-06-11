#include "private.h"

#include "json_write.h"

#include <string.h>
#include <stdlib.h>

static void snapshot_copy_cstr(char *out, size_t out_len, const char *value)
{
    size_t len;

    if (out == NULL || out_len == 0u) {
        return;
    }
    if (value == NULL) {
        value = "";
    }

    len = strlen(value);
    if (len >= out_len) {
        len = out_len - 1u;
    }
    memcpy(out, value, len);
    out[len] = '\0';
}

static int snapshot_add_string_array32(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key,
    const char values[][32],
    size_t value_count
)
{
    yyjson_mut_val *arr;
    size_t i;
    int rc;

    arr = json_writer_add_array(writer, obj, key);
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < value_count; i++) {
        rc = json_writer_array_add_string(writer, arr, values[i]);
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int snapshot_add_adjustments(
    json_writer_t *writer,
    yyjson_mut_val *line_obj,
    const stitched_service_line_t *line
)
{
    yyjson_mut_val *arr;
    yyjson_mut_val *adjustment_obj;
    const stitched_line_adjustment_t *adjustment;
    size_t i;
    int rc;

    arr = json_writer_add_array(writer, line_obj, "adjustments");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < line->adjustment_count; i++) {
        adjustment = &line->adjustments[i];
        adjustment_obj = json_writer_array_add_object(writer, arr);
        if (adjustment_obj == NULL) {
            return X12_ERR_NO_MEMORY;
        }

        rc = json_writer_add_string(
            writer,
            adjustment_obj,
            "adjustment_group_code",
            adjustment->adjustment_group_code
        );
        if (rc == X12_OK) {
            rc = snapshot_add_string_array32(
                writer,
                adjustment_obj,
                "reason_codes",
                adjustment->reason_codes,
                adjustment->value_count
            );
        }
        if (rc == X12_OK) {
            rc = snapshot_add_string_array32(
                writer,
                adjustment_obj,
                "amounts",
                adjustment->amounts,
                adjustment->value_count
            );
        }
        if (rc == X12_OK) {
            rc = snapshot_add_string_array32(
                writer,
                adjustment_obj,
                "quantities",
                adjustment->quantities,
                adjustment->value_count
            );
        }
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int snapshot_add_submitted_line(
    json_writer_t *writer,
    yyjson_mut_val *line_obj,
    const stitched_service_line_t *line
)
{
    yyjson_mut_val *obj;
    int rc;

    if (!line->has_submitted) {
        return X12_OK;
    }

    obj = json_writer_add_object(writer, line_obj, "submitted");
    if (obj == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = json_writer_add_string(writer, obj, "line_type", line->submitted_line_type);
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "procedure_code_qualifier",
            line->submitted_procedure_code_qualifier
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "procedure_code_set",
            line->submitted_procedure_code_set
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "charge_amount", line->submitted_charge_amount);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "unit_measure_code",
            line->submitted_unit_measure_code
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "unit_count", line->submitted_unit_count);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "service_date", line->submitted_service_date);
    }

    return rc;
}

static int snapshot_add_remittance_line(
    json_writer_t *writer,
    yyjson_mut_val *line_obj,
    const stitched_service_line_t *line
)
{
    yyjson_mut_val *obj;
    int rc;

    if (!line->has_remittance) {
        return X12_OK;
    }

    obj = json_writer_add_object(writer, line_obj, "remittance");
    if (obj == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = json_writer_add_string(
        writer,
        obj,
        "procedure_code_qualifier",
        line->remittance_procedure_code_qualifier
    );
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "procedure_code_set",
            line->remittance_procedure_code_set
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "line_charge_amount",
            line->remittance_line_charge_amount
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "line_paid_amount",
            line->remittance_line_paid_amount
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "paid_service_unit_count",
            line->remittance_paid_unit_count
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "service_date", line->remittance_service_date);
    }

    return rc;
}

static int snapshot_add_service_lines(
    json_writer_t *writer,
    yyjson_mut_val *root,
    const claim_aggregate_t *aggregate
)
{
    yyjson_mut_val *arr;
    yyjson_mut_val *line_obj;
    const stitched_service_line_t *line;
    size_t i;
    int rc;

    arr = json_writer_add_array(writer, root, "service_lines");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < aggregate->service_line_count; i++) {
        line = &aggregate->service_lines[i];
        line_obj = json_writer_array_add_object(writer, arr);
        if (line_obj == NULL) {
            return X12_ERR_NO_MEMORY;
        }

        rc = json_writer_add_string(writer, line_obj, "service_line_number", line->service_line_number);
        if (rc == X12_OK) {
            rc = json_writer_add_string(
                writer,
                line_obj,
                "remittance_service_line_number",
                line->remit_service_line_number
            );
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, line_obj, "procedure_code", line->procedure_code);
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, line_obj, "description", line->description);
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, line_obj, "service_date", line->service_date);
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, line_obj, "match_method", line->match_method);
        }
        if (rc == X12_OK) {
            rc = snapshot_add_submitted_line(writer, line_obj, line);
        }
        if (rc == X12_OK) {
            rc = snapshot_add_remittance_line(writer, line_obj, line);
        }
        if (rc == X12_OK) {
            rc = snapshot_add_adjustments(writer, line_obj, line);
        }
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int snapshot_add_source_event_ids(
    json_writer_t *writer,
    yyjson_mut_val *root,
    const claim_aggregate_t *aggregate
)
{
    yyjson_mut_val *arr;
    size_t i;
    int rc;

    arr = json_writer_add_array(writer, root, "applied_event_ids");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < aggregate->source_event_count; i++) {
        rc = json_writer_array_add_size(writer, arr, aggregate->source_events[i].event_id);
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int snapshot_add_update_event_ids(
    json_writer_t *writer,
    yyjson_mut_val *root,
    const stitch_update_batch_t *batch
)
{
    yyjson_mut_val *arr;
    size_t i;
    size_t end;
    int rc;

    if (batch == NULL || batch->aggregate == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    arr = json_writer_add_array(writer, root, "update_event_ids");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    end = batch->first_source_event_index + batch->source_event_count;
    for (i = batch->first_source_event_index; i < end; i++) {
        rc = json_writer_array_add_size(writer, arr, batch->aggregate->source_events[i].event_id);
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int build_snapshot_doc(
    const stitch_state_t *state,
    const stitch_update_batch_t *batch,
    const char *aggregate_id,
    json_writer_t *writer,
    int include_contains_phi,
    int include_lineage,
    int include_event_ids
)
{
    const claim_aggregate_t *aggregate;
    yyjson_mut_val *root;
    yyjson_mut_val *keys;
    yyjson_mut_val *snapshot_state;
    yyjson_mut_val *lineage;
    int include_phi;
    int rc;
    char claim_id[STITCH_ID_MAX];
    char claim_id_token[TOKENISE_MAX_TOKEN_LEN];
    char payer_control[STITCH_ID_MAX];
    char payer_control_token[TOKENISE_MAX_TOKEN_LEN];

    if (state == NULL || batch == NULL || batch->aggregate == NULL ||
        aggregate_id == NULL || writer == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    aggregate = batch->aggregate;
    include_phi = state->include_phi;

    rc = json_writer_init_object(writer);
    if (rc != X12_OK) {
        return rc;
    }

    root = json_writer_root(writer);
    rc = json_writer_add_string(writer, root, "event_type", "ClaimAggregateUpdated");
    if (rc == X12_OK && state->run_id[0] != '\0') {
        rc = json_writer_add_string(writer, root, "run_id", state->run_id);
    }
    if (rc == X12_OK && batch->source_run_id[0] != '\0') {
        rc = json_writer_add_string(writer, root, "source_run_id", batch->source_run_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, root, "aggregate_type", "claim");
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, root, "aggregate_id", aggregate_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(writer, root, "version", aggregate->version);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(writer, root, "updated_by_event_id", batch->updated_by_event_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, root, "updated_by_event_type", batch->updated_by_event_type);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, root, "update_scope", "source_drop");
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, root, "source_drop_id", batch->source_drop_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(
            writer,
            root,
            "compacted_source_event_count",
            batch->source_event_count
        );
    }
    if (rc == X12_OK && include_contains_phi) {
        rc = json_writer_add_bool(writer, root, "contains_phi", include_phi);
    }
    if (rc != X12_OK) {
        return rc;
    }

    keys = json_writer_add_object(writer, root, "keys");
    if (keys == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = claim_stitch_resolve_identifier_output_pair(
        state,
        "claim_id",
        aggregate->claim_id,
        aggregate->claim_id_token,
        claim_id,
        sizeof(claim_id),
        claim_id_token,
        sizeof(claim_id_token)
    );
    if (rc != X12_OK) {
        return rc;
    }
    if (include_phi && claim_id[0] != '\0') {
        rc = json_writer_add_string(writer, keys, "claim_id", claim_id);
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, keys, "claim_id_token", claim_id_token);
        }
    } else {
        rc = json_writer_add_string(
            writer,
            keys,
            "claim_id",
            claim_id_token[0] != '\0' ? claim_id_token : aggregate->key
        );
    }
    if (rc != X12_OK) {
        return rc;
    }

    rc = claim_stitch_resolve_identifier_output_pair(
        state,
        "payer_claim_control_number",
        aggregate->payer_claim_control_number,
        aggregate->payer_claim_control_number_token,
        payer_control,
        sizeof(payer_control),
        payer_control_token,
        sizeof(payer_control_token)
    );
    if (rc != X12_OK) {
        return rc;
    }
    if (payer_control[0] != '\0' || payer_control_token[0] != '\0') {
        if (include_phi && payer_control[0] != '\0') {
            rc = json_writer_add_string(
                writer,
                keys,
                "payer_claim_control_number",
                payer_control
            );
            if (rc == X12_OK) {
                rc = json_writer_add_string(
                    writer,
                    keys,
                    "payer_claim_control_number_token",
                    payer_control_token
                );
            }
        } else {
            rc = json_writer_add_string(
                writer,
                keys,
                "payer_claim_control_number",
                payer_control_token
            );
        }
        if (rc != X12_OK) {
            return rc;
        }
    }
    if (include_phi && aggregate->patient_id[0] != '\0') {
        rc = json_writer_add_string(writer, keys, "patient_id", aggregate->patient_id);
        if (rc == X12_OK && aggregate->patient_id_token[0] != '\0') {
            rc = json_writer_add_string(
                writer,
                keys,
                "patient_id_token",
                aggregate->patient_id_token
            );
        }
    } else if (aggregate->patient_id_token[0] != '\0') {
        rc = json_writer_add_string(writer, keys, "patient_id", aggregate->patient_id_token);
    }
    if (rc != X12_OK) {
        return rc;
    }
    if (include_phi && aggregate->patient_name[0] != '\0') {
        char patient_last_name_or_org[STITCH_VALUE_MAX];
        char patient_first_name[STITCH_VALUE_MAX];

        claim_stitch_split_patient_name(
            aggregate->patient_name,
            patient_last_name_or_org,
            sizeof(patient_last_name_or_org),
            patient_first_name,
            sizeof(patient_first_name)
        );
        rc = json_writer_add_string(
            writer,
            keys,
            "patient_last_name_or_org",
            patient_last_name_or_org
        );
        if (rc == X12_OK && patient_first_name[0] != '\0') {
            rc = json_writer_add_string(writer, keys, "patient_first_name", patient_first_name);
        }
        if (rc == X12_OK && aggregate->patient_name_token[0] != '\0') {
            rc = json_writer_add_string(
                writer,
                keys,
                "patient_name_token",
                aggregate->patient_name_token
            );
        }
    } else if (aggregate->patient_name_token[0] != '\0') {
        rc = json_writer_add_string(writer, keys, "patient_name_token", aggregate->patient_name_token);
    }
    if (rc != X12_OK) {
        return rc;
    }

    snapshot_state = json_writer_add_object(writer, root, "state");
    if (snapshot_state == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = json_writer_add_bool(writer, snapshot_state, "has_837", aggregate->has_837);
    if (rc == X12_OK) {
        rc = json_writer_add_bool(writer, snapshot_state, "has_835", aggregate->has_835);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, snapshot_state, "claim_type", aggregate->claim_type);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            snapshot_state,
            "claim_status_code",
            aggregate->claim_status_code
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(
            writer,
            snapshot_state,
            "source_event_count",
            aggregate->source_event_count
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(
            writer,
            snapshot_state,
            "submitted_service_line_count",
            aggregate->submitted_service_line_count
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(
            writer,
            snapshot_state,
            "remittance_service_line_count",
            aggregate->remittance_service_line_count
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(
            writer,
            snapshot_state,
            "adjustment_count",
            aggregate->adjustment_count
        );
    }
    if (rc == X12_OK) {
        rc = snapshot_add_service_lines(writer, root, aggregate);
    }
    if (rc != X12_OK) {
        return rc;
    }

    if (include_lineage) {
        lineage = json_writer_add_object(writer, root, "lineage");
        if (lineage == NULL) {
            return X12_ERR_NO_MEMORY;
        }
        rc = json_writer_add_size(
            writer,
            lineage,
            "applied_event_count",
            aggregate->source_event_count
        );
        if (rc == X12_OK) {
            rc = json_writer_add_size(
                writer,
                lineage,
                "update_event_count",
                batch->source_event_count
            );
        }
        if (rc != X12_OK) {
            return rc;
        }
    }
    if (include_event_ids) {
        rc = snapshot_add_source_event_ids(writer, root, aggregate);
        if (rc == X12_OK) {
            rc = snapshot_add_update_event_ids(writer, root, batch);
        }
    }

    return rc;
}

static int build_snapshot_state_json(
    const stitch_state_t *state,
    const stitch_update_batch_t *batch,
    const char *aggregate_id,
    char *out,
    size_t out_len
)
{
    json_writer_t writer = {0};
    int rc;

    if (out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    out[0] = '\0';

    rc = build_snapshot_doc(
        state,
        batch,
        aggregate_id,
        &writer,
        1,
        1,
        0
    );
    if (rc == X12_OK) {
        rc = json_writer_write_cstring(&writer, out, out_len);
    }
    json_writer_free(&writer);
    return rc;
}

static int persist_claim_aggregate_keys(
    stitch_state_t *state,
    const claim_aggregate_t *aggregate,
    const char *aggregate_id
)
{
    int rc;

    if (state == NULL || state->read_store == NULL || aggregate == NULL ||
        aggregate_id == NULL) {
        return X12_OK;
    }

    rc = scribe_store_put_claim_aggregate_key(
        state->read_store,
        "claim_id",
        aggregate->key,
        aggregate_id
    );
    if (rc != X12_OK) {
        return rc;
    }
    if (aggregate->claim_id_token[0] != '\0') {
        rc = scribe_store_put_claim_aggregate_key(
            state->read_store,
            "claim_id_token",
            aggregate->claim_id_token,
            aggregate_id
        );
        if (rc != X12_OK) {
            return rc;
        }
    }
    if (state->include_phi && aggregate->claim_id[0] != '\0') {
        rc = scribe_store_put_claim_aggregate_key(
            state->read_store,
            "claim_id_raw",
            aggregate->claim_id,
            aggregate_id
        );
        if (rc != X12_OK) {
            return rc;
        }
    }
    if (aggregate->payer_claim_control_number_token[0] != '\0') {
        rc = scribe_store_put_claim_aggregate_key(
            state->read_store,
            "payer_claim_control_number",
            aggregate->payer_claim_control_number_token,
            aggregate_id
        );
        if (rc != X12_OK) {
            return rc;
        }
    }
    if (aggregate->payer_claim_control_number[0] != '\0') {
        rc = scribe_store_put_claim_aggregate_key(
            state->read_store,
            state->include_phi ? "payer_claim_control_number_raw" : "payer_claim_control_number",
            aggregate->payer_claim_control_number,
            aggregate_id
        );
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int persist_snapshot_to_read_store(
    stitch_state_t *state,
    const stitch_update_batch_t *batch,
    const char *aggregate_id
)
{
    char *state_json;
    char updated_by_event_id[32];
    int written;
    int rc;

    if (state == NULL || state->read_store == NULL) {
        return X12_OK;
    }

    state_json = (char *)malloc(STITCH_STATE_JSON_MAX);
    if (state_json == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = build_snapshot_state_json(
        state,
        batch,
        aggregate_id,
        state_json,
        STITCH_STATE_JSON_MAX
    );
    if (rc != X12_OK) {
        free(state_json);
        return rc;
    }

    written = snprintf(
        updated_by_event_id,
        sizeof(updated_by_event_id),
        "%zu",
        batch->updated_by_event_id
    );
    if (written < 0 || (size_t)written >= sizeof(updated_by_event_id)) {
        free(state_json);
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    rc = scribe_store_put_claim_aggregate(
        state->read_store,
        aggregate_id,
        batch->aggregate->version,
        state_json,
        updated_by_event_id,
        batch->source_drop_id
    );
    if (rc == X12_OK) {
        rc = persist_claim_aggregate_keys(state, batch->aggregate, aggregate_id);
    }
    free(state_json);
    return rc;
}

static int write_notification(
    stitch_state_t *state,
    const stitch_update_batch_t *batch,
    const char *aggregate_id
)
{
    json_writer_t writer = {0};
    yyjson_mut_val *root;
    char notification_id[STITCH_ID_MAX + 64u];
    int written;
    int rc;

    if (state == NULL || batch == NULL || batch->aggregate == NULL ||
        aggregate_id == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (state->notify_out == NULL) {
        return X12_OK;
    }

    written = snprintf(
        notification_id,
        sizeof(notification_id),
        "%s:%zu",
        aggregate_id,
        batch->aggregate->version
    );
    if (written < 0 || (size_t)written >= sizeof(notification_id)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    rc = json_writer_init_object(&writer);
    if (rc != X12_OK) {
        return rc;
    }
    root = json_writer_root(&writer);

    rc = json_writer_add_string(&writer, root, "event_type", "AggregateVersionRecorded");
    if (rc == X12_OK) {
        rc = json_writer_add_bool(&writer, root, "ok", 1);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(&writer, root, "notification_id", notification_id);
    }
    if (rc == X12_OK && state->run_id[0] != '\0') {
        rc = json_writer_add_string(&writer, root, "run_id", state->run_id);
    }
    if (rc == X12_OK && batch->source_run_id[0] != '\0') {
        rc = json_writer_add_string(&writer, root, "source_run_id", batch->source_run_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(&writer, root, "aggregate_type", "claim");
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(&writer, root, "aggregate_id", aggregate_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(&writer, root, "version", batch->aggregate->version);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(&writer, root, "source_drop_id", batch->source_drop_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(&writer, root, "updated_by_event_id", batch->updated_by_event_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            &writer,
            root,
            "updated_by_event_type",
            batch->updated_by_event_type
        );
    }
    if (rc == X12_OK && batch->updated_by_journal_offset >= 0) {
        rc = json_writer_add_size(
            &writer,
            root,
            "updated_by_journal_offset",
            (size_t)batch->updated_by_journal_offset
        );
    }
    if (rc == X12_OK && batch->updated_by_journal_length >= 0) {
        rc = json_writer_add_size(
            &writer,
            root,
            "updated_by_journal_length",
            (size_t)batch->updated_by_journal_length
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_write_fp(&writer, state->notify_out, 1);
    }
    json_writer_free(&writer);

    return rc;
}

static int write_snapshot(
    stitch_state_t *state,
    const stitch_update_batch_t *batch
)
{
    json_writer_t writer = {0};
    char aggregate_id[STITCH_ID_MAX + 16u];
    int written;
    int rc;

    if (state == NULL || batch == NULL || batch->aggregate == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    written = snprintf(
        aggregate_id,
        sizeof(aggregate_id),
        "claim:%s",
        batch->aggregate->key
    );
    if (written < 0 || (size_t)written >= sizeof(aggregate_id)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    fprintf(
        stderr,
        "scribe stitch claims: emit aggregate=%s version=%zu source_drop=%s update_events=%zu\n",
        aggregate_id,
        batch->aggregate->version,
        batch->source_drop_id,
        batch->source_event_count
    );

    rc = build_snapshot_doc(
        state,
        batch,
        aggregate_id,
        &writer,
        0,
        0,
        1
    );
    if (rc == X12_OK) {
        rc = json_writer_write_fp(&writer, state->out, 1);
    }
    json_writer_free(&writer);
    if (rc != X12_OK) {
        return rc;
    }

    rc = persist_snapshot_to_read_store(state, batch, aggregate_id);
    if (rc != X12_OK) {
        return rc;
    }
    if (state->incremental && state->read_store != NULL) {
        rc = scribe_store_clear_dirty_aggregate(
            state->read_store,
            "claim",
            aggregate_id,
            batch->source_drop_id
        );
        if (rc != X12_OK) {
            return rc;
        }
    }

    return write_notification(state, batch, aggregate_id);
}

static int snapshot_get_string(
    yyjson_val *obj,
    const char *key,
    char *out,
    size_t out_len
)
{
    yyjson_val *value;
    const char *str;
    size_t len;

    if (out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    out[0] = '\0';
    if (obj == NULL || key == NULL) {
        return X12_OK;
    }

    value = yyjson_obj_get(obj, key);
    if (value == NULL || !yyjson_is_str(value)) {
        return X12_OK;
    }

    str = yyjson_get_str(value);
    len = yyjson_get_len(value);
    if (len >= out_len) {
        len = out_len - 1u;
    }
    memcpy(out, str, len);
    out[len] = '\0';
    return X12_OK;
}

static void snapshot_get_bool(yyjson_val *obj, const char *key, int *out)
{
    yyjson_val *value;

    if (out == NULL || obj == NULL || key == NULL) {
        return;
    }

    value = yyjson_obj_get(obj, key);
    if (value != NULL && yyjson_is_bool(value)) {
        *out = yyjson_get_bool(value) ? 1 : 0;
    }
}

static void snapshot_get_size(yyjson_val *obj, const char *key, size_t *out)
{
    yyjson_val *value;

    if (out == NULL || obj == NULL || key == NULL) {
        return;
    }

    value = yyjson_obj_get(obj, key);
    if (value != NULL && yyjson_is_uint(value)) {
        *out = (size_t)yyjson_get_uint(value);
    }
}

static int hydrate_adjustments(
    stitched_service_line_t *line,
    yyjson_val *line_obj
)
{
    yyjson_val *arr;
    yyjson_val *item;
    yyjson_val *values;
    yyjson_val *value;
    size_t idx;
    size_t max;
    size_t value_idx;
    size_t value_max;

    arr = yyjson_obj_get(line_obj, "adjustments");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    yyjson_arr_foreach(arr, idx, max, item) {
        stitched_line_adjustment_t *adjustment;

        if (!yyjson_is_obj(item)) {
            continue;
        }
        if (line->adjustment_count >= STITCH_MAX_ADJUSTMENTS_PER_LINE) {
            return X12_ERR_NO_MEMORY;
        }

        adjustment = &line->adjustments[line->adjustment_count++];
        (void)snapshot_get_string(
            item,
            "adjustment_group_code",
            adjustment->adjustment_group_code,
            sizeof(adjustment->adjustment_group_code)
        );

        values = yyjson_obj_get(item, "reason_codes");
        if (values != NULL && yyjson_is_arr(values)) {
            yyjson_arr_foreach(values, value_idx, value_max, value) {
                if (adjustment->value_count >= STITCH_MAX_ADJUSTMENT_VALUES) {
                    break;
                }
                if (yyjson_is_str(value)) {
                    snapshot_copy_cstr(
                        adjustment->reason_codes[adjustment->value_count],
                        sizeof(adjustment->reason_codes[adjustment->value_count]),
                        yyjson_get_str(value)
                    );
                    adjustment->value_count++;
                }
            }
        }

        values = yyjson_obj_get(item, "amounts");
        if (values != NULL && yyjson_is_arr(values)) {
            yyjson_arr_foreach(values, value_idx, value_max, value) {
                if (value_idx >= STITCH_MAX_ADJUSTMENT_VALUES) {
                    break;
                }
                if (yyjson_is_str(value)) {
                    snapshot_copy_cstr(
                        adjustment->amounts[value_idx],
                        sizeof(adjustment->amounts[value_idx]),
                        yyjson_get_str(value)
                    );
                    if (value_idx >= adjustment->value_count) {
                        adjustment->value_count = value_idx + 1u;
                    }
                }
            }
        }

        values = yyjson_obj_get(item, "quantities");
        if (values != NULL && yyjson_is_arr(values)) {
            yyjson_arr_foreach(values, value_idx, value_max, value) {
                if (value_idx >= STITCH_MAX_ADJUSTMENT_VALUES) {
                    break;
                }
                if (yyjson_is_str(value)) {
                    snapshot_copy_cstr(
                        adjustment->quantities[value_idx],
                        sizeof(adjustment->quantities[value_idx]),
                        yyjson_get_str(value)
                    );
                    if (value_idx >= adjustment->value_count) {
                        adjustment->value_count = value_idx + 1u;
                    }
                }
            }
        }
    }

    return X12_OK;
}

static int hydrate_service_lines(claim_aggregate_t *aggregate, yyjson_val *root)
{
    yyjson_val *arr;
    yyjson_val *item;
    size_t idx;
    size_t max;
    int rc;

    arr = yyjson_obj_get(root, "service_lines");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    yyjson_arr_foreach(arr, idx, max, item) {
        stitched_service_line_t *line;
        yyjson_val *submitted;
        yyjson_val *remittance;

        if (!yyjson_is_obj(item)) {
            continue;
        }
        if (aggregate->service_line_count >= STITCH_MAX_LINES_PER_CLAIM) {
            return X12_ERR_NO_MEMORY;
        }

        line = &aggregate->service_lines[aggregate->service_line_count++];
        rc = snapshot_get_string(
            item,
            "service_line_number",
            line->service_line_number,
            sizeof(line->service_line_number)
        );
        if (rc == X12_OK) {
            rc = snapshot_get_string(
                item,
                "remittance_service_line_number",
                line->remit_service_line_number,
                sizeof(line->remit_service_line_number)
            );
        }
        if (rc == X12_OK) {
            rc = snapshot_get_string(item, "procedure_code", line->procedure_code, sizeof(line->procedure_code));
        }
        if (rc == X12_OK) {
            rc = snapshot_get_string(item, "description", line->description, sizeof(line->description));
        }
        if (rc == X12_OK) {
            rc = snapshot_get_string(item, "service_date", line->service_date, sizeof(line->service_date));
        }
        if (rc == X12_OK) {
            rc = snapshot_get_string(item, "match_method", line->match_method, sizeof(line->match_method));
        }
        if (rc != X12_OK) {
            return rc;
        }

        submitted = yyjson_obj_get(item, "submitted");
        if (submitted != NULL && yyjson_is_obj(submitted)) {
            line->has_submitted = 1;
            (void)snapshot_get_string(submitted, "line_type", line->submitted_line_type, sizeof(line->submitted_line_type));
            (void)snapshot_get_string(submitted, "procedure_code_qualifier", line->submitted_procedure_code_qualifier, sizeof(line->submitted_procedure_code_qualifier));
            (void)snapshot_get_string(submitted, "procedure_code_set", line->submitted_procedure_code_set, sizeof(line->submitted_procedure_code_set));
            (void)snapshot_get_string(submitted, "charge_amount", line->submitted_charge_amount, sizeof(line->submitted_charge_amount));
            (void)snapshot_get_string(submitted, "unit_measure_code", line->submitted_unit_measure_code, sizeof(line->submitted_unit_measure_code));
            (void)snapshot_get_string(submitted, "unit_count", line->submitted_unit_count, sizeof(line->submitted_unit_count));
            (void)snapshot_get_string(submitted, "service_date", line->submitted_service_date, sizeof(line->submitted_service_date));
        }

        remittance = yyjson_obj_get(item, "remittance");
        if (remittance != NULL && yyjson_is_obj(remittance)) {
            line->has_remittance = 1;
            (void)snapshot_get_string(remittance, "procedure_code_qualifier", line->remittance_procedure_code_qualifier, sizeof(line->remittance_procedure_code_qualifier));
            (void)snapshot_get_string(remittance, "procedure_code_set", line->remittance_procedure_code_set, sizeof(line->remittance_procedure_code_set));
            (void)snapshot_get_string(remittance, "line_charge_amount", line->remittance_line_charge_amount, sizeof(line->remittance_line_charge_amount));
            (void)snapshot_get_string(remittance, "line_paid_amount", line->remittance_line_paid_amount, sizeof(line->remittance_line_paid_amount));
            (void)snapshot_get_string(remittance, "paid_service_unit_count", line->remittance_paid_unit_count, sizeof(line->remittance_paid_unit_count));
            (void)snapshot_get_string(remittance, "service_date", line->remittance_service_date, sizeof(line->remittance_service_date));
        }

        rc = hydrate_adjustments(line, item);
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

int claim_stitch_hydrate_snapshot(
    stitch_state_t *state,
    const char *aggregate_id,
    const char *state_json
)
{
    yyjson_doc *doc;
    yyjson_val *root;
    yyjson_val *keys;
    yyjson_val *snapshot_state;
    claim_aggregate_t *aggregate;
    const char *key;
    int rc = X12_OK;

    if (state == NULL || aggregate_id == NULL || state_json == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (state->aggregate_count >= STITCH_MAX_AGGREGATES) {
        return X12_ERR_NO_MEMORY;
    }

    key = strncmp(aggregate_id, "claim:", 6u) == 0 ? aggregate_id + 6u : aggregate_id;
    aggregate = &state->aggregates[state->aggregate_count++];
    memset(aggregate, 0, sizeof(*aggregate));
    snapshot_copy_cstr(aggregate->key, sizeof(aggregate->key), key);

    doc = yyjson_read(state_json, strlen(state_json), 0);
    if (doc == NULL) {
        state->aggregate_count--;
        return X12_ERR_INVALID_ARGUMENT;
    }
    root = yyjson_doc_get_root(doc);
    if (root == NULL || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        state->aggregate_count--;
        return X12_ERR_INVALID_ARGUMENT;
    }

    snapshot_get_size(root, "version", &aggregate->version);
    keys = yyjson_obj_get(root, "keys");
    if (keys != NULL && yyjson_is_obj(keys)) {
        (void)snapshot_get_string(keys, "claim_id", aggregate->claim_id, sizeof(aggregate->claim_id));
        (void)snapshot_get_string(keys, "claim_id_token", aggregate->claim_id_token, sizeof(aggregate->claim_id_token));
        (void)snapshot_get_string(keys, "payer_claim_control_number", aggregate->payer_claim_control_number, sizeof(aggregate->payer_claim_control_number));
        (void)snapshot_get_string(keys, "payer_claim_control_number_token", aggregate->payer_claim_control_number_token, sizeof(aggregate->payer_claim_control_number_token));
        (void)snapshot_get_string(keys, "patient_id", aggregate->patient_id, sizeof(aggregate->patient_id));
        (void)snapshot_get_string(keys, "patient_id_token", aggregate->patient_id_token, sizeof(aggregate->patient_id_token));
        (void)snapshot_get_string(keys, "patient_name_token", aggregate->patient_name_token, sizeof(aggregate->patient_name_token));
    }
    if (aggregate->claim_id_token[0] == '\0') {
        snapshot_copy_cstr(aggregate->claim_id_token, sizeof(aggregate->claim_id_token), aggregate->key);
    }

    snapshot_state = yyjson_obj_get(root, "state");
    if (snapshot_state != NULL && yyjson_is_obj(snapshot_state)) {
        snapshot_get_bool(snapshot_state, "has_837", &aggregate->has_837);
        snapshot_get_bool(snapshot_state, "has_835", &aggregate->has_835);
        (void)snapshot_get_string(snapshot_state, "claim_type", aggregate->claim_type, sizeof(aggregate->claim_type));
        (void)snapshot_get_string(snapshot_state, "claim_status_code", aggregate->claim_status_code, sizeof(aggregate->claim_status_code));
        snapshot_get_size(snapshot_state, "submitted_service_line_count", &aggregate->submitted_service_line_count);
        snapshot_get_size(snapshot_state, "remittance_service_line_count", &aggregate->remittance_service_line_count);
        snapshot_get_size(snapshot_state, "adjustment_count", &aggregate->adjustment_count);
    }

    rc = hydrate_service_lines(aggregate, root);
    yyjson_doc_free(doc);
    if (rc != X12_OK) {
        state->aggregate_count--;
    }
    return rc;
}

int claim_stitch_flush_update_batches(stitch_state_t *state)
{
    size_t i;
    int rc;

    if (state == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    for (i = 0u; i < state->update_batch_count; i++) {
        if (state->update_batches[i].source_event_count == 0u) {
            continue;
        }
        rc = write_snapshot(state, &state->update_batches[i]);
        if (rc != X12_OK) {
            return rc;
        }
    }

    state->update_batch_count = 0u;
    return X12_OK;
}
