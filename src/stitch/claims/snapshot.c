#include "private.h"

#include "json_write.h"

#include <stdlib.h>

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

    return write_notification(state, batch, aggregate_id);
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
