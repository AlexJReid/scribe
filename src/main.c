#include "aggregate_stitcher.h"
#include "balance_projector.h"
#include "coverage_stitcher.h"
#include "delta_exporter.h"
#include "event_writer.h"
#include "journal_builder.h"
#include "phi_vault.h"
#include "run_id.h"
#include "scribe_version.h"
#include "x12_mapper_registry.h"
#include "x12_reader.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE *fp)
{
    fputs(
        "usage examples:\n"
        "  scribe parse --type 270 input.edi [--out events.ndjson] [--include-phi] [--run-id id]\n"
        "  scribe parse --type 271 input.edi [--out events.ndjson] [--include-phi] [--run-id id]\n"
        "  scribe parse --type 837 input.edi [--out events.ndjson] [--include-phi] [--run-id id]\n"
        "  scribe parse --type 835 input.edi [--out events.ndjson] [--include-phi] [--run-id id]\n"
        "  scribe parse --type 834 input.edi [--out events.ndjson] [--include-phi] [--run-id id]\n"
        "  scribe ingest --out journal.scribe [--append] [--source-root dir] [--compress zstd] [--zstd-level n] [--270 inquiry.edi] [--270-list paths.txt] [--271 response.edi] [--271-list paths.txt] [--834 enroll.edi] [--834-list paths.txt] [--837 claim.edi] [--837-list paths.txt] [--835 remit.edi] [--835-list paths.txt] [--phi-vault phi.sqlite] [--include-phi] [--run-id id]\n"
        "  scribe stitch claims --journal journal.scribe [--incremental] [--read-store store.sqlite] [--phi-vault phi.sqlite] [--out aggregates.ndjson] [--include-phi] [--run-id id]\n"
        "  scribe stitch coverage --journal journal.scribe [--incremental] [--read-store store.sqlite] [--phi-vault phi.sqlite] [--out coverage.ndjson] [--include-phi] [--run-id id]\n"
        "  scribe export delta --read-store store.sqlite --out delta.sqlite [--after-sequence n] [--limit n]\n"
        "  scribe project balance --read-store store.sqlite [--out balance.json] [--include-phi]\n"
        "  scribe vault resolve --phi-vault phi.sqlite --namespace ns --token token [--actor user] [--purpose reason]\n"
        "  scribe dump input.edi\n"
        "  scribe --version\n"
        "\n"
        "For parse/stitch/project, --out may be '-' or omitted for stdout. ingest requires a journal file path.\n"
        "\n"
        "available projections:\n"
        "  balance - claim -> service line ledger balance\n",
        fp);
}

static void version(void)
{
    printf("scribe %s (%s)\n", SCRIBE_VERSION, SCRIBE_BUILD_INFO);
}

static int dump_segment(const x12_segment_t *seg, void *user)
{
    (void)user;

    printf(
        "%06zu %.*s elements=%zu\n",
        seg->segment_index,
        (int)seg->tag.len,
        seg->tag.ptr,
        seg->element_count);

    return X12_OK;
}

static int run_dump(const char *input_path)
{
    x12_document_t doc;
    int rc;

    rc = x12_document_load(input_path, &doc);
    if (rc != X12_OK)
    {
        fprintf(stderr, "load %s: %s\n", input_path, x12_error_message(rc));
        return 1;
    }

    rc = x12_document_each_segment(&doc, dump_segment, NULL);
    if (rc != X12_OK)
    {
        fprintf(stderr, "dump %s: %s\n", input_path, x12_error_message(rc));
        x12_document_free(&doc);
        return 1;
    }

    x12_document_free(&doc);
    return 0;
}

static int run_parse(
    const char *type,
    const char *input_path,
    const char *out_path,
    int include_phi,
    const char *run_id)
{
    x12_document_t doc;
    event_writer_t writer;
    char generated_run_id[96];
    int rc;
    int close_rc;

    if (run_id == NULL || run_id[0] == '\0')
    {
        rc = scribe_run_id_generate(generated_run_id, sizeof(generated_run_id));
        if (rc != X12_OK)
        {
            fprintf(stderr, "run id: %s\n", x12_error_message(rc));
            return 1;
        }
        run_id = generated_run_id;
    }

    rc = x12_document_load(input_path, &doc);
    if (rc != X12_OK)
    {
        fprintf(stderr, "load %s: %s\n", input_path, x12_error_message(rc));
        return 1;
    }

    rc = event_writer_open(&writer, out_path, input_path, type);
    if (rc != X12_OK)
    {
        fprintf(stderr, "open output: %s\n", x12_error_message(rc));
        x12_document_free(&doc);
        return 1;
    }
    event_writer_set_include_phi(&writer, include_phi);
    event_writer_set_run_id(&writer, run_id);

    {
        x12_mapper_fn map = x12_mapper_for_type(type);
        rc = map != NULL ? map(&doc, &writer) : X12_ERR_UNSUPPORTED;
    }

    close_rc = event_writer_close(&writer);
    x12_document_free(&doc);

    if (rc != X12_OK)
    {
        fprintf(stderr, "parse %s: %s\n", input_path, x12_error_message(rc));
        return 1;
    }
    if (close_rc != X12_OK)
    {
        fprintf(stderr, "close output: %s\n", x12_error_message(close_rc));
        return 1;
    }

    return 0;
}

static int parse_command(int argc, char **argv)
{
    const char *type = NULL;
    const char *input_path = NULL;
    const char *out_path = "-";
    const char *run_id = NULL;
    int include_phi = 0;
    int i;

    for (i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "--type") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            type = argv[++i];
        }
        else if (strcmp(argv[i], "--out") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            out_path = argv[++i];
        }
        else if (strcmp(argv[i], "--include-phi") == 0)
        {
            include_phi = 1;
        }
        else if (strcmp(argv[i], "--run-id") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            run_id = argv[++i];
        }
        else if (input_path == NULL)
        {
            input_path = argv[i];
        }
        else
        {
            return -1;
        }
    }

    if (type == NULL || input_path == NULL)
    {
        return -1;
    }

    return run_parse(type, input_path, out_path, include_phi, run_id);
}

static int ingest_command(int argc, char **argv, const char *command_name)
{
    int rc = journal_builder_run_cli(argc, argv);

    if (rc < 0)
    {
        if (rc != -1)
        {
            fprintf(stderr, "%s: %s\n", command_name, x12_error_message(rc));
        }
        return rc;
    }

    return rc;
}

static int project_balance_command(int argc, char **argv, const char *command_name)
{
    int rc;

    rc = balance_projector_run_cli(argc, argv);
    if (rc < 0 && rc != -1)
    {
        fprintf(stderr, "%s: %s\n", command_name, x12_error_message(rc));
    }
    return rc;
}

static int project_command(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[2], "balance") == 0)
    {
        return project_balance_command(argc - 1, argv + 1, "project balance");
    }

    return -1;
}

static int stitch_claims_command(int argc, char **argv, const char *command_name)
{
    int rc;

    rc = aggregate_stitcher_run_cli(argc, argv);
    if (rc < 0 && rc != -1)
    {
        fprintf(stderr, "%s: %s\n", command_name, x12_error_message(rc));
    }
    return rc;
}

static int stitch_coverage_command(int argc, char **argv, const char *command_name)
{
    int rc;

    rc = coverage_stitcher_run_cli(argc, argv);
    if (rc < 0 && rc != -1)
    {
        fprintf(stderr, "%s: %s\n", command_name, x12_error_message(rc));
    }
    return rc;
}

static int stitch_command(int argc, char **argv)
{
    if (argc < 3)
    {
        return -1;
    }

    if (strcmp(argv[2], "claims") == 0)
    {
        return stitch_claims_command(argc - 1, argv + 1, "stitch claims");
    }
    if (strcmp(argv[2], "coverage") == 0)
    {
        return stitch_coverage_command(argc - 1, argv + 1, "stitch coverage");
    }

    return -1;
}

static int vault_resolve_command(int argc, char **argv);

static int export_delta_command(int argc, char **argv, const char *command_name)
{
    int rc;

    rc = scribe_delta_exporter_run_cli(argc, argv);
    if (rc < 0 && rc != -1)
    {
        fprintf(stderr, "%s: %s\n", command_name, x12_error_message(rc));
    }
    return rc;
}

static int export_command(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[2], "delta") == 0)
    {
        return export_delta_command(argc - 1, argv + 1, "export delta");
    }

    return -1;
}

static int vault_command(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[2], "resolve") == 0)
    {
        return vault_resolve_command(argc - 1, argv + 1);
    }

    return -1;
}

static int vault_resolve_command(int argc, char **argv)
{
    const char *vault_path = NULL;
    const char *namespace_name = NULL;
    const char *token = NULL;
    const char *actor = "cli";
    const char *purpose = "debug";
    char resolved[1024];
    phi_vault_t vault;
    int i;
    int rc;

    for (i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "--phi-vault") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            vault_path = argv[++i];
        }
        else if (strcmp(argv[i], "--namespace") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            namespace_name = argv[++i];
        }
        else if (strcmp(argv[i], "--token") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            token = argv[++i];
        }
        else if (strcmp(argv[i], "--actor") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            actor = argv[++i];
        }
        else if (strcmp(argv[i], "--purpose") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            purpose = argv[++i];
        }
        else
        {
            return -1;
        }
    }

    if (vault_path == NULL || namespace_name == NULL || token == NULL)
    {
        return -1;
    }

    phi_vault_init(&vault);
    rc = phi_vault_open(&vault, vault_path);
    if (rc != X12_OK)
    {
        fprintf(stderr, "vault open: %s\n", x12_error_message(rc));
        return rc;
    }
    rc = phi_vault_init_schema(&vault);
    if (rc == X12_OK)
    {
        rc = phi_vault_resolve(
            &vault,
            namespace_name,
            token,
            actor,
            purpose,
            resolved,
            sizeof(resolved));
    }
    if (phi_vault_close(&vault) != X12_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }
    if (rc != X12_OK)
    {
        fprintf(stderr, "vault resolve: %s\n", x12_error_message(rc));
        return rc;
    }

    puts(resolved);
    return X12_OK;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        usage(stderr);
        return 1;
    }

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
    {
        version();
        return 0;
    }

    if (strcmp(argv[1], "dump") == 0)
    {
        if (argc != 3)
        {
            usage(stderr);
            return 1;
        }
        return run_dump(argv[2]);
    }

    if (strcmp(argv[1], "parse") == 0)
    {
        int rc = parse_command(argc, argv);
        if (rc < 0)
        {
            usage(stderr);
            return 1;
        }
        return rc;
    }

    if (strcmp(argv[1], "ingest") == 0)
    {
        int rc = ingest_command(argc, argv, argv[1]);
        if (rc < 0)
        {
            usage(stderr);
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "stitch") == 0)
    {
        int rc = stitch_command(argc, argv);
        if (rc < 0)
        {
            usage(stderr);
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "export") == 0)
    {
        int rc = export_command(argc, argv);
        if (rc < 0)
        {
            usage(stderr);
            return 1;
        }
        return rc == X12_OK ? 0 : 1;
    }

    if (strcmp(argv[1], "vault") == 0)
    {
        int rc = vault_command(argc, argv);
        if (rc == -1)
        {
            usage(stderr);
            return 1;
        }
        return rc == X12_OK ? 0 : 1;
    }

    if (strcmp(argv[1], "project") == 0)
    {
        int rc = project_command(argc, argv);
        if (rc < 0)
        {
            usage(stderr);
            return 1;
        }
        return 0;
    }

    usage(stderr);
    return 1;
}
