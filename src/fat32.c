#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/stdlib/string.h"
#include "header/filesystem/fat32.h"

const uint8_t fs_signature[BLOCK_SIZE] = {
    'C', 'o', 'u', 'r', 's', 'e', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',  ' ',
    'D', 'e', 's', 'i', 'g', 'n', 'e', 'd', ' ', 'b', 'y', ' ', ' ', ' ', ' ',  ' ',
    'L', 'a', 'b', ' ', 'S', 'i', 's', 't', 'e', 'r', ' ', 'I', 'T', 'B', ' ',  ' ',
    'M', 'a', 'd', 'e', ' ', 'w', 'i', 't', 'h', ' ', '<', '3', ' ', ' ', ' ',  ' ',
    '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '2', '0', '2', '4', '\n',
    [BLOCK_SIZE - 2] = 'O',
    [BLOCK_SIZE - 1] = 'k',
};

static struct FAT32DriverState driver_state = { 0 };

/**
 * Convert cluster number to logical block address
 *
 * @param cluster Cluster number to convert
 * @return uint32_t Logical Block Address
 */
uint32_t cluster_to_lba(uint32_t cluster) {
    return cluster * CLUSTER_BLOCK_COUNT;
}

/**
 * Initialize DirectoryTable value with
 * - Entry-0: DirectoryEntry about itself
 * - Entry-1: Parent DirectoryEntry
 *
 * @param dir_table          Pointer to directory table
 * @param name               8-byte char for directory name
 * @param parent_dir_cluster Parent directory cluster number
 */
void init_directory_table(struct FAT32DirectoryTable* dir_table, char* name, uint32_t parent_dir_cluster) {
    uint16_t cluster_low = parent_dir_cluster & 0xFFFF;
    uint16_t cluster_high = (parent_dir_cluster >> 16) & 0xFFFF;
    dir_table->table[0].cluster_low = cluster_low;
    dir_table->table[0].cluster_high = cluster_high;
    dir_table->table[0].user_attribute = UATTR_NOT_EMPTY;
    dir_table->table[0].attribute = ATTR_SUBDIRECTORY;
    memcpy(dir_table->table[0].name, name, 8);
}

/**
 * Checking whether filesystem signature is missing or not in boot sector
 *
 * @return True if memcmp(boot_sector, fs_signature) returning inequality
 */
bool is_empty_storage(void) {
    struct BlockBuffer boot_sector;
    read_blocks(&boot_sector, BOOT_SECTOR, 1);
    return memcmp(&boot_sector, fs_signature, BLOCK_SIZE);
}


/**
 * Create new FAT32 file system. Will write fs_signature into boot sector and
 * proper FileAllocationTable (contain CLUSTER_0_VALUE, CLUSTER_1_VALUE,
 * and initialized root directory) into cluster number 1
 */
void create_fat32(void) {
    write_blocks(fs_signature, BOOT_SECTOR, 1);

    driver_state.fat_table.cluster_map[0] = CLUSTER_0_VALUE;
    driver_state.fat_table.cluster_map[1] = CLUSTER_1_VALUE;
    driver_state.fat_table.cluster_map[ROOT_CLUSTER_NUMBER] = FAT32_FAT_END_OF_FILE;

    for (uint16_t i = 3; i < CLUSTER_MAP_SIZE; i++) {
        driver_state.fat_table.cluster_map[i] = FAT32_FAT_EMPTY_ENTRY;
    }

    write_clusters(&driver_state.fat_table, FAT_CLUSTER_NUMBER, 1);

    struct FAT32DirectoryTable root_dir_table = { 0 };
    init_directory_table(&root_dir_table, "root", ROOT_CLUSTER_NUMBER);
    write_clusters(&root_dir_table, ROOT_CLUSTER_NUMBER, 1);
}

/**
 * Initialize file system driver state, if is_empty_storage() then create_fat32()
 * Else, read and cache entire FileAllocationTable (located at cluster number 1) into driver state
 */
void initialize_filesystem_fat32(void) {
    if (is_empty_storage()) {
        create_fat32();
    }
    else {
        read_clusters(&driver_state.fat_table, FAT_CLUSTER_NUMBER, 1);
    }
}

/**
 * Write cluster operation, wrapper for write_blocks().
 * Recommended to use struct ClusterBuffer
 *
 * @param ptr            Pointer to source data
 * @param cluster_number Cluster number to write
 * @param cluster_count  Cluster count to write, due limitation of write_blocks block_count 255 => max cluster_count = 63
 */
void write_clusters(const void* ptr, uint32_t cluster_number, uint8_t cluster_count) {
    write_blocks(ptr, cluster_to_lba(cluster_number), cluster_count * CLUSTER_BLOCK_COUNT);
}

/**
 * Read cluster operation, wrapper for read_blocks().
 * Recommended to use struct ClusterBuffer
 *
 * @param ptr            Pointer to buffer for reading
 * @param cluster_number Cluster number to read
 * @param cluster_count  Cluster count to read, due limitation of read_blocks block_count 255 => max cluster_count = 63
 */
void read_clusters(void* ptr, uint32_t cluster_number, uint8_t cluster_count) {
    read_blocks(ptr, cluster_to_lba(cluster_number), cluster_count * CLUSTER_BLOCK_COUNT);
}


/* -- CRUD Operation -- */

/**
 *  FAT32 Folder / Directory read
 *
 * @param request buf point to struct FAT32DirectoryTable,
 *                name is directory name,
 *                ext is unused,
 *                parent_cluster_number is target directory table to read,
 *                buffer_size must be exactly sizeof(struct FAT32DirectoryTable)
 * @return Error code: 0 success - 1 not a folder - 2 not found - -1 unknown
 */
int8_t read_directory(struct FAT32DriverRequest request) {
    read_clusters(&driver_state.dir_table_buf, request.parent_cluster_number, 1);

    // Check if parent directory is a folder
    if (driver_state.dir_table_buf.table[0].attribute != ATTR_SUBDIRECTORY) {
        return -1;
    }

    // Loop over entries in directory table
    uint32_t directory_size = sizeof(struct FAT32DirectoryTable) / sizeof(struct FAT32DirectoryEntry);
    for (uint32_t i = 0; i < directory_size; i++) {
        // Check if name and extension is match
        bool is_name_match = !memcmp(driver_state.dir_table_buf.table[i].name, request.name, 8);
        bool is_ext_match = !memcmp(driver_state.dir_table_buf.table[i].ext, request.ext, 3);

        if (is_name_match && is_ext_match) {
            // Check if is a directory
            bool is_directory = driver_state.dir_table_buf.table[i].attribute == ATTR_SUBDIRECTORY;
            if (!is_directory) {
                return 1;
            }

            // Read directory table
            uint32_t cluster_number = driver_state.dir_table_buf.table[i].cluster_low | (driver_state.dir_table_buf.table[i].cluster_high << 16);
            read_clusters(&driver_state.dir_table_buf, cluster_number, 1);
            return 0;
        }
    }

    return 2;
}


/**
 * FAT32 read, read a file from file system.
 *
 * @param request All attribute will be used for read, buffer_size will limit reading count
 * @return Error code: 0 success - 1 not a file - 2 not enough buffer - 3 not found - -1 unknown
 */
int8_t read(struct FAT32DriverRequest request) {
    read_clusters(&driver_state.dir_table_buf, request.parent_cluster_number, 1);

    // Check if parent directory is a folder
    if (driver_state.dir_table_buf.table[0].attribute != ATTR_SUBDIRECTORY) {
        return -1;
    }

    // Loop over entries in directory table
    uint32_t directory_size = sizeof(struct FAT32DirectoryTable) / sizeof(struct FAT32DirectoryEntry);
    for (uint32_t i = 0; i < directory_size; i++) {
        // Check if name and extension is match
        bool is_name_match = !memcmp(driver_state.dir_table_buf.table[i].name, request.name, 8);
        bool is_ext_match = !memcmp(driver_state.dir_table_buf.table[i].ext, request.ext, 3);

        if (is_name_match && is_ext_match) {
            // Check if is a file
            bool is_file = driver_state.dir_table_buf.table[i].attribute != ATTR_SUBDIRECTORY;
            if (!is_file) {
                return 1;
            }

            // Check if buffer size is enough
            bool is_buffer_enough = request.buffer_size >= driver_state.dir_table_buf.table[i].filesize;
            if (!is_buffer_enough) {
                return 2;
            }

            // Read file content
            uint32_t cluster_number = driver_state.dir_table_buf.table[i].cluster_low | (driver_state.dir_table_buf.table[i].cluster_high << 16);
            uint32_t offset = 0;

            do {
                read_clusters(request.buf + offset * CLUSTER_SIZE, cluster_number, 1);
                cluster_number = driver_state.fat_table.cluster_map[cluster_number];
                offset++;
            } while (cluster_number != FAT32_FAT_END_OF_FILE);

            return 0;
        }
    }

    return 3;
}

int32_t ceil_div(int32_t a, int32_t b) {
    return a / b + (a % b != 0);
}

/**
 * FAT32 write, write a file or folder to file system.
 *
 * @param request All attribute will be used for write, buffer_size == 0 then create a folder / directory
 * @return Error code: 0 success - 1 file/folder already exist - 2 invalid parent cluster - -1 unknown
 */
int8_t write(struct FAT32DriverRequest request) {
    read_clusters(&driver_state.dir_table_buf, request.parent_cluster_number, 1);

    // Check if parent directory is a folder
    if (driver_state.dir_table_buf.table[0].attribute != ATTR_SUBDIRECTORY) {
        return -1;
    }

    // Loop over entries in directory table
    uint32_t directory_size = sizeof(struct FAT32DirectoryTable) / sizeof(struct FAT32DirectoryEntry);
    for (uint32_t i = 0; i < directory_size; i++) {
        // Check if name and extension is match
        bool is_name_match = !memcmp(driver_state.dir_table_buf.table[i].name, request.name, 8);
        bool is_ext_match = !memcmp(driver_state.dir_table_buf.table[i].ext, request.ext, 3);

        if (is_name_match && is_ext_match) {
            return 1;
        }
    }

    // Check if amount of cluster is enough
    uint32_t cluster_count = ceil_div(request.buffer_size, CLUSTER_SIZE);
    uint32_t cluster_available = 0;
    for (uint32_t i = 2; i < CLUSTER_MAP_SIZE; i++) {
        if (driver_state.fat_table.cluster_map[i] == FAT32_FAT_EMPTY_ENTRY) {
            cluster_available++;
        }
    }

    if (cluster_available < cluster_count) {
        return -1;
    }

    uint32_t empty_cluster = 0;
    for (uint32_t i = 2; i < CLUSTER_MAP_SIZE; i++) {
        if (driver_state.fat_table.cluster_map[i] == FAT32_FAT_EMPTY_ENTRY) {
            empty_cluster = i;
            break;
        }
    }

    // Write file content
    struct FAT32DirectoryEntry new_entry = { .filesize = request.buffer_size, .user_attribute = UATTR_NOT_EMPTY };
    memcpy(new_entry.name, request.name, 8);
    memcpy(new_entry.ext, request.ext, 3);
    new_entry.cluster_low = empty_cluster & 0xFFFF;
    new_entry.cluster_high = (empty_cluster >> 16) & 0xFFFF;

    if (request.buffer_size == 0) {
        new_entry.attribute = ATTR_SUBDIRECTORY;
        struct FAT32DirectoryTable new_dir_table = { 0 };
        init_directory_table(&new_dir_table, request.name, request.parent_cluster_number);
        driver_state.fat_table.cluster_map[empty_cluster] = FAT32_FAT_END_OF_FILE;
        write_clusters(&new_dir_table, empty_cluster, 1);
    }
    else {
        uint32_t empty_clusters[CLUSTER_MAP_SIZE] = { 0 };
        uint32_t idx = 0;
        for (uint32_t i = 0; i < CLUSTER_MAP_SIZE; i++) {
            if (driver_state.fat_table.cluster_map[i] == FAT32_FAT_EMPTY_ENTRY) {
                empty_clusters[idx++] = i;
            }
        }

        for (uint32_t i = 0; i < cluster_count; i++) {
            uint32_t cluster_number = empty_clusters[i];
            if (i == cluster_count - 1) {
                driver_state.fat_table.cluster_map[cluster_number] = FAT32_FAT_END_OF_FILE;
            }
            else {
                driver_state.fat_table.cluster_map[cluster_number] = empty_clusters[i + 1];
            }
            write_clusters(request.buf + i * CLUSTER_SIZE, cluster_number, 1);
        }
    }

    driver_state.dir_table_buf.table[directory_size] = new_entry;
    write_clusters(&driver_state.dir_table_buf, request.parent_cluster_number, 1);
    write_clusters(&driver_state.fat_table, FAT_CLUSTER_NUMBER, 1);

    return 0;
}


/**
 * FAT32 delete, delete a file or empty directory (only 1 DirectoryEntry) in file system.
 *
 * @param request buf and buffer_size is unused
 * @return Error code: 0 success - 1 not found - 2 folder is not empty - -1 unknown
 */
int8_t delete(struct FAT32DriverRequest request) {
    read_clusters(&driver_state.dir_table_buf, request.parent_cluster_number, 1);

    // Check if parent directory is a folder
    if (driver_state.dir_table_buf.table[0].attribute != ATTR_SUBDIRECTORY) {
        return -1;
    }

    // Loop over entries in directory table
    uint32_t directory_size = sizeof(struct FAT32DirectoryTable) / sizeof(struct FAT32DirectoryEntry);
    for (uint32_t i = 0; i < directory_size; i++) {
        // Check if name and extension is match
        bool is_name_match = !memcmp(driver_state.dir_table_buf.table[i].name, request.name, 8);
        bool is_ext_match = !memcmp(driver_state.dir_table_buf.table[i].ext, request.ext, 3);

        if (is_name_match && is_ext_match) {
            struct FAT32DirectoryEntry entry = driver_state.dir_table_buf.table[i];

            if (entry.attribute == ATTR_SUBDIRECTORY) {
                struct FAT32DirectoryTable dir_table;
                uint32_t cluster_number = entry.cluster_low | (entry.cluster_high << 16);
                read_clusters(&dir_table, cluster_number, 1);

                for (uint32_t i = 1; i < directory_size; i++) {
                    if (dir_table.table[i].user_attribute == UATTR_NOT_EMPTY) {
                        return 2;
                    }
                }
            }

            // Remove entry
            driver_state.dir_table_buf.table[i].user_attribute = 0;
            memset(driver_state.dir_table_buf.table[i].name, 0, 8);
            memset(driver_state.dir_table_buf.table[i].ext, 0, 3);

            // Remove file content
            uint32_t cluster_number = entry.cluster_low | (entry.cluster_high << 16);
            do {
                uint32_t next_cluster = driver_state.fat_table.cluster_map[cluster_number];
                driver_state.fat_table.cluster_map[cluster_number] = FAT32_FAT_EMPTY_ENTRY;
                cluster_number = next_cluster;
            } while (cluster_number != FAT32_FAT_END_OF_FILE);

            write_clusters(&driver_state.dir_table_buf, request.parent_cluster_number, 1);
            write_clusters(&driver_state.fat_table, FAT_CLUSTER_NUMBER, 1);

            return 0;
        }
    }

    return 1;
}