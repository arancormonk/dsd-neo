// SPDX-License-Identifier: ISC
#include <dsd-neo/core/dsd.h>
#define BSIZE 999

int
csvGroupImport(dsd_opts* opts, dsd_state* state) {
    char filename[1024] = "filename.csv";
    sprintf(filename, "%s", opts->group_in_file);
    //filename[1023] = '\0'; //necessary?
    char buffer[BSIZE];
    FILE* fp;
    fp = fopen(filename, "r");
    if (fp == NULL) {
        printf("Unable to open group file '%s'\n", filename);
        exit(1);
    }
    int row_count = 0;
    int field_count = 0;
    long int group_number = 0; //local group number for array index value
    UNUSED(group_number);
    int i = 0;
    while (fgets(buffer, BSIZE, fp)) {
        field_count = 0;
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        char* field = strtok(buffer, ","); //seperate by comma
        while (field) {

            if (field_count == 0) {
                //group_number = atol(field);
                state->group_array[i].groupNumber = atol(field);
                fprintf(stderr, "%ld, ", state->group_array[i].groupNumber);
            }
            if (field_count == 1) {
                strncpy(state->group_array[i].groupMode, field, sizeof(state->group_array[i].groupMode) - 1);
                state->group_array[i].groupMode[sizeof(state->group_array[i].groupMode) - 1] = '\0';
                fprintf(stderr, "%s, ", state->group_array[i].groupMode);
            }
            if (field_count == 2) {
                strncpy(state->group_array[i].groupName, field, sizeof(state->group_array[i].groupName) - 1);
                state->group_array[i].groupName[sizeof(state->group_array[i].groupName) - 1] = '\0';
                fprintf(stderr, "%s ", state->group_array[i].groupName);
            }

            field = strtok(NULL, ",");
            field_count++;
        }
        fprintf(stderr, "\n");
        i++;
        state->group_tally++;
    }
    fclose(fp);
    return 0;
}

//LCN import for EDACS, migrated to channel map (channel map does both)
int
csvLCNImport(dsd_opts* opts, dsd_state* state) {
    char filename[1024] = "filename.csv";
    sprintf(filename, "%s", opts->lcn_in_file);
    //filename[1023] = '\0'; //necessary?
    char buffer[BSIZE];
    FILE* fp;
    fp = fopen(filename, "r");
    if (fp == NULL) {
        printf("Unable to open lcn file '%s'\n", filename);
        //have this return -1 and handle it inside of main
        exit(1);
    }
    int row_count = 0;
    int field_count = 0;

    while (fgets(buffer, BSIZE, fp)) {
        field_count = 0;
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        char* field = strtok(buffer, ","); //seperate by comma
        while (field) {

            state->trunk_lcn_freq[field_count] = atol(field);
            state->lcn_freq_count++; //keep tally of number of Frequencies imported
            fprintf(stderr, "LCN [%d] [%ld]", field_count + 1, state->trunk_lcn_freq[field_count]);
            fprintf(stderr, "\n");

            field = strtok(NULL, ",");
            field_count++;
        }
        fprintf(stderr, "LCN Count %d\n", state->lcn_freq_count);
    }
    fclose(fp);
    return 0;
}

int
csvChanImport(dsd_opts* opts, dsd_state* state) //channel map import
{
    char filename[1024] = "filename.csv";
    sprintf(filename, "%s", opts->chan_in_file);

    char buffer[BSIZE];
    FILE* fp;
    fp = fopen(filename, "r");
    if (fp == NULL) {
        printf("Unable to open channel map file '%s'\n", filename);
        //have this return -1 and handle it inside of main
        exit(1);
    }
    int row_count = 0;
    int field_count = 0;

    long int chan_number;

    while (fgets(buffer, BSIZE, fp)) {
        field_count = 0;
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        char* field = strtok(buffer, ","); //seperate by comma
        while (field) {

            if (field_count == 0) {
                sscanf(field, "%ld", &chan_number);
            }

            if (field_count == 1) {
                sscanf(field, "%ld", &state->trunk_chan_map[chan_number]);
                //adding this should be compatible with EDACS, test and obsolete the LCN Import function if desired
                sscanf(field, "%ld", &state->trunk_lcn_freq[state->lcn_freq_count]);
                state->lcn_freq_count++; //keep tally of number of Frequencies imported
            }

            field = strtok(NULL, ",");
            field_count++;
        }
        fprintf(stderr, "Channel [%05ld] [%09ld]", chan_number, state->trunk_chan_map[chan_number]);
        fprintf(stderr, "\n");
    }
    fclose(fp);
    return 0;
}

//Decimal Variant of Key Import
int
csvKeyImportDec(dsd_opts* opts, dsd_state* state) //multi-key support
{
    char filename[1024] = "filename.csv";
    sprintf(filename, "%s", opts->key_in_file);

    char buffer[BSIZE];
    FILE* fp;
    fp = fopen(filename, "r");
    if (fp == NULL) {
        printf("Unable to open file '%s'\n", filename);
        exit(1);
    }
    int row_count = 0;
    int field_count = 0;

    unsigned long long int keynumber = 0;
    unsigned long long int keyvalue = 0;

    uint16_t hash = 0;
    uint8_t hash_bits[24];
    memset(hash_bits, 0, sizeof(hash_bits));

    while (fgets(buffer, BSIZE, fp)) {
        field_count = 0;
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        char* field = strtok(buffer, ","); //seperate by comma
        while (field) {

            if (field_count == 0) {
                sscanf(field, "%lld", &keynumber);
                if (keynumber > 0xFFFF) //if larger than 16-bits, get its hash instead
                {
                    keynumber = keynumber & 0xFFFFFF; //truncate to 24-bits (max allowed)
                    for (int i = 0; i < 24; i++) {
                        hash_bits[i] = ((keynumber << i) & 0x800000) >> 23; //load into array for CRC16
                    }
                    hash = ComputeCrcCCITT16d(hash_bits, 24);
                    keynumber = hash & 0xFFFF; //make sure its no larger than 16-bits
                    fprintf(stderr, "Hashed ");
                }
            }

            if (field_count == 1) {
                sscanf(field, "%lld", &keyvalue);
                state->rkey_array[keynumber] = keyvalue & 0xFFFFFFFFFF; //doesn't exceed 40-bit value
            }

            field = strtok(NULL, ",");
            field_count++;
        }
        fprintf(stderr, "Key [%03lld] [%05lld]", keynumber, state->rkey_array[keynumber]);
        fprintf(stderr, "\n");
        hash = 0;
    }
    fclose(fp);
    return 0;
}

//Hex Variant of Key Import
int
csvKeyImportHex(dsd_opts* opts, dsd_state* state) //key import for hex keys
{
    char filename[1024] = "filename.csv";
    sprintf(filename, "%s", opts->key_in_file);
    char buffer[BSIZE];
    FILE* fp;
    fp = fopen(filename, "r");
    if (fp == NULL) {
        printf("Unable to open file '%s'\n", filename);
        exit(1);
    }
    int row_count = 0;
    int field_count = 0;
    unsigned long long int keynumber;

    while (fgets(buffer, BSIZE, fp)) {
        field_count = 0;
        row_count++;
        if (row_count == 1) {
            continue; //don't want labels
        }
        char* field = strtok(buffer, ","); //seperate by comma
        while (field) {

            if (field_count == 0) {
                sscanf(field, "%llX", &keynumber);
            }

            if (field_count == 1) {
                sscanf(field, "%llX", &state->rkey_array[keynumber]);
            }

            //this could also theoretically nuke other keys that are at the same offset
            if (field_count == 2) {
                sscanf(field, "%llX", &state->rkey_array[keynumber + 0x101]);
            }

            if (field_count == 3) {
                sscanf(field, "%llX", &state->rkey_array[keynumber + 0x201]);
            }

            if (field_count == 4) {
                sscanf(field, "%llX", &state->rkey_array[keynumber + 0x301]);
            }

            field = strtok(NULL, ",");
            field_count++;
        }

        fprintf(stderr, "Key [%04llX] [%016llX]", keynumber, state->rkey_array[keynumber]);

        //if longer key is loaded (or clash with the 0x101, 0x201, 0x301 offset, then print the full key listing)
        if ((state->rkey_array[keynumber + 0x101] != 0) || (state->rkey_array[keynumber + 0x201] != 0)
            || (state->rkey_array[keynumber + 0x301] != 0)) {
            fprintf(stderr, " [%016llX] [%016llX] [%016llX]", state->rkey_array[keynumber + 0x101],
                    state->rkey_array[keynumber + 0x201], state->rkey_array[keynumber + 0x301]);
        }
        fprintf(stderr, "\n");
    }
    fclose(fp);
    return 0;
}
