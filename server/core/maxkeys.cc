/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-04-23
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxscale/ccdefs.hh>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <getopt.h>
#include <pwd.h>
#include <random>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include <maxbase/log.hh>
#include <maxscale/paths.hh>
#include "internal/secrets.hh"

using std::string;

struct option options[] =
{
    {"help",  no_argument,       nullptr, 'h'},
    {"user",  required_argument, nullptr, 'u'},
    {nullptr, 0,                 nullptr, 0  }
};

const string default_user = "maxscale";

bool           write_keys(const EncryptionKeys& key, const string& filepath, const string& owner);
EncryptionKeys gen_random_key();

void print_usage(const char* executable, const char* default_directory)
{
    const char msg[] =
        R"(usage: %s [-h|--help] [directory]

        This utility generates a random AES encryption key and init vector and writes them to disk.
        The data is written to the file '%s', in the specified directory. The key and init vector
        are used by the utility 'maxpasswd' to encrypt passwords used in MaxScale configuration files,
        as well as by MaxScale itself to decrypt the passwords.

        Re-creating the file invalidates all existing encrypted passwords in the configuration files.

         -h, --help    Display this help
         -u, --user    Designate the owner of the generated file (default: '%s')

        directory  : The directory where the file should be created in (default: '%s')
)";
    printf(msg, executable, SECRETS_FILENAME, default_user.c_str(), default_directory);
}

int main(int argc, char** argv)
{
    mxb::Log log(MXB_LOG_TARGET_STDOUT);
    const string default_directory = mxs::datadir();
    string username = default_user;

    int c;
    while ((c = getopt_long(argc, argv, "h", options, nullptr)) != -1)
    {
        switch (c)
        {
        case 'h':
            print_usage(argv[0], default_directory.c_str());
            return EXIT_SUCCESS;

        case 'u':
            username = optarg;
            break;

        default:
            print_usage(argv[0], default_directory.c_str());
            return EXIT_FAILURE;
        }
    }

    string filepath = default_directory;
    if (optind < argc)
    {
        filepath = argv[optind];
    }
    filepath.append("/").append(SECRETS_FILENAME);

    // Check that the file doesn't exist.
    errno = 0;
    auto filepathz = filepath.c_str();
    if (access(filepathz, F_OK) == 0)
    {
        printf("Secrets file '%s' already exists. Delete it before generating a new encryption key.\n",
               filepathz);
        return EXIT_FAILURE;
    }
    else if (errno != ENOENT)
    {
        printf("stat() for secrets file '%s' failed unexpectedly. Error %i, %s.\n",
               filepathz, errno, mxb_strerror(errno));
        return EXIT_FAILURE;
    }

    int rval = EXIT_FAILURE;
    auto new_key = gen_random_key();
    if (write_keys(new_key, filepath, username))
    {
        rval = EXIT_SUCCESS;
    }
    return rval;
}

/**
 * Write encryption key and init vector to binary file. Also sets file permissions and owner.
 *
 * @param key Key data
 * @param filepath The full path to the file to write to.
 * @param owner The final owner of the file. Changing the owner may not succeed.
 * @return True on total success. Even if false is returned, the file may have been written.
 */
bool write_keys(const EncryptionKeys& key, const string& filepath, const string& owner)
{
    auto filepathz = filepath.c_str();
    bool write_ok = false;
    {
        errno = 0;
        std::ofstream file(filepath, std::ios_base::binary);
        if (file.is_open())
        {
            file.write((const char*)key.enckey, EncryptionKeys::key_len);
            file.write((const char*)key.initvector, EncryptionKeys::iv_len);
            if (file.good())
            {
                write_ok = true;
                printf("Encryption key written to secrets file '%s'.\n", filepathz);
            }
            else
            {
                printf("Write to secrets file '%s' failed. Error %d, %s.\n",
                       filepathz, errno, mxs_strerror(errno));
            }
            file.close();
        }
        else
        {
            printf("Could not open secrets file '%s' for writing. Error %d, %s.\n",
                   filepathz, errno, mxs_strerror(errno));
        }
    }

    bool rval = false;
    if (write_ok)
    {
        // Change file permissions to prevent modifications.
        errno = 0;
        if (chmod(filepathz, S_IRUSR) == 0)
        {
            printf("Permissions of '%s' set to owner:read.\n", filepathz);
            auto ownerz = owner.c_str();
            auto userinfo = getpwnam(ownerz);
            if (userinfo)
            {
                if (chown(filepathz, userinfo->pw_uid, userinfo->pw_gid) == 0)
                {
                    printf("Ownership of '%s' given to %s.\n", filepathz, ownerz);
                    rval = true;
                }
                else
                {
                    printf("Failed to give '%s' ownership of '%s': %d, %s.\n",
                           ownerz, filepathz, errno, mxb_strerror(errno));
                }
            }
            else
            {
                printf("Could not find user '%s' when attempting to change ownership of '%s': %d, %s.\n",
                       ownerz, filepathz, errno, mxb_strerror(errno));
            }
        }
        else
        {
            printf("Failed to change the permissions of the secrets file '%s'. Error %d, %s.\n",
                   filepathz, errno, mxs_strerror(errno));
        }
    }
    return rval;
}

EncryptionKeys gen_random_key()
{
    const auto buflen = EncryptionKeys::total_len;
    uint8_t rand_buffer[buflen];

    // Need 48 bytes of random data. Generate it using OpenSSL.
    if (RAND_bytes(rand_buffer, buflen) < 1)
    {
        printf("OpenSSL RAND_bytes() failed. %s.\n", ERR_error_string(0, nullptr));
        // Failed to get proper random bytes, perhaps the system doesn't have a valid random source.
        // Use a time-seeded pseudorandom generator as backup.
        std::mt19937_64 engine;
        auto timepoint = std::chrono::system_clock::now();
        engine.seed(timepoint.time_since_epoch().count());
        printf("Using a random number generator seeded from current time as backup.\n");

        int bytes_written = 0;
        while (bytes_written < buflen)
        {
            auto rand_number = engine();
            int new_bytes = sizeof(rand_number);
            int bytes_to_write = std::min(new_bytes, buflen - bytes_written);
            memcpy(rand_buffer + bytes_written, &rand_number, bytes_to_write);
            bytes_written += bytes_to_write;
        }
    }
    EncryptionKeys key;
    memcpy(key.enckey, rand_buffer, EncryptionKeys::key_len);
    memcpy(key.initvector, rand_buffer + EncryptionKeys::key_len, EncryptionKeys::iv_len);
    return key;
}
