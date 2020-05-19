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

/**
 * @file maxpasswd.c  - Implementation of pasword encoding
 */

#include <maxscale/ccdefs.hh>

#include <cstdio>
#include <getopt.h>
#include <maxbase/log.hh>
#include <maxscale/paths.hh>

#include "internal/secrets.hh"

using std::string;

struct option options[] =
{
    {"help",  no_argument, nullptr, 'h'},
    {nullptr, 0,           nullptr, 0  }
};

void print_usage(const char* executable, const char* directory)
{
    const char msg[] =
        R"(Usage: %s [-h|--help] [path] password

Encrypt a MaxScale plaintext password using the encryption key in the key file
'%s'. The key file may be generated using the 'maxkeys'-utility.

  -h, --help  Display this help.

  path        The directory where the key file is located (default: '%s')
  password    The plaintext password to encrypt
)";
    printf(msg, executable, SECRETS_FILENAME, directory);
}

int main(int argc, char** argv)
{
    mxb::Log log(MXB_LOG_TARGET_STDOUT);
    const char* default_directory = mxs::datadir();

    int c;
    while ((c = getopt_long(argc, argv, "h", options, NULL)) != -1)
    {
        switch (c)
        {
        case 'h':
            print_usage(argv[0], default_directory);
            return EXIT_SUCCESS;

        default:
            print_usage(argv[0], default_directory);
            return EXIT_FAILURE;
        }
    }

    string plaintext_pw;
    string path = default_directory;

    switch (argc - optind)
    {
    case 2:
        // Two args provided.
        path = argv[optind];
        plaintext_pw = argv[optind + 1];
        break;

    case 1:
        // One arg provided.
        plaintext_pw = argv[optind];
        break;

    default:
        print_usage(argv[0], default_directory);
        return EXIT_FAILURE;
    }

    int rval = EXIT_FAILURE;
    string filepath = path;
    filepath.append("/").append(SECRETS_FILENAME);

    auto keys = secrets_readkeys(filepath);
    if (keys.ok)
    {
        if (keys.key)
        {
            std::string enc = encrypt_password(*keys.key, plaintext_pw);
            if (!enc.empty())
            {
                printf("%s\n", enc.c_str());
                rval = EXIT_SUCCESS;
            }
            else
            {
                printf("Password encryption failed.\n");
            }
        }
        else
        {
            printf("Password encryption key file '%s' not found, cannot encrypt password.\n",
                   filepath.c_str());
        }
    }
    else
    {
        printf("Could not read encryption key file '%s'.\n", filepath.c_str());
    }
    return rval;
}
