#include "zlib.h"
#include <assert.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <openssl/evp.h>
#include <sstream>
#include <string>
#define CHUNK 16384

void cat_file(int, char **);
int inf(FILE *);
int hash_object(char *);
std::string digestToString(unsigned char *digest, unsigned int len);

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "No command provided.\n";
    return EXIT_FAILURE;
  }

  std::string command = argv[1];

  if (command == "init") {
    try {
      std::filesystem::create_directory(".git");
      std::filesystem::create_directory(".git/objects");
      std::filesystem::create_directory(".git/refs");

      std::ofstream headFile(".git/HEAD");
      if (headFile.is_open()) {
        headFile << "ref: refs/heads/main\n";
        headFile.close();
      } else {
        std::cerr << "Failed to create .git/HEAD file.\n";
        return EXIT_FAILURE;
      }

      std::cout << "Initialized git directory\n";
    } catch (const std::filesystem::filesystem_error &e) {
      std::cerr << e.what() << '\n';
      return EXIT_FAILURE;
    }
  } else if (command == "cat-file") {
    cat_file(argc, argv);
  } else if (command == "hash-object") {
    if (argc == 3)
      hash_object(argv[2]);
    else
      std::cout << "git hash-object <file name>" << std::endl;
  } else {
    std::cerr << "Unknown command " << command << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

void cat_file(int argc, char **argv) {
  if (argc < 3) {
    std::cout << "Too few arguments" << std::endl;
    return;
  }
  std::string flag = argv[2];
  if (flag == "-p") {
    if (argc == 4) {
      std::string blob_sha = argv[3];
      blob_sha.insert(2, "/");
      blob_sha.insert(0, ".git/objects/");
      FILE *source = fopen(&blob_sha[0], "r");
      inf(source);
      fclose(source);
    } else {
      std::cout << "cli cat-file -p <blob_sha>" << std::endl;
    }
  } else {
    // TODO print out the content compressed content of the blob file
    std::cout << "content: " << argv[2] << std::endl;
  }
}

int inf(FILE *source) {
  bool header_found = false;
  size_t header_offset = 0;
  int ret;
  unsigned have;
  z_stream strm;
  unsigned char in[CHUNK];
  unsigned char out[CHUNK];
  /* allocate inflate state */
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  strm.avail_in = 0;
  strm.next_in = Z_NULL;
  ret = inflateInit(&strm);
  if (ret != Z_OK)
    return ret;

  /* decompress until deflate stream ends or end of file */
  do {
    strm.avail_in = fread(in, 1, CHUNK, source);
    if (ferror(source)) {
      (void)inflateEnd(&strm);
      return Z_ERRNO;
    }
    if (strm.avail_in == 0)
      break;
    strm.next_in = in;
    /* run inflate() on input until output buffer not full */
    do {
      strm.avail_out = CHUNK;
      strm.next_out = out;
      ret = inflate(&strm, Z_NO_FLUSH);
      assert(ret != Z_STREAM_ERROR); /* state not clobbered */
      switch (ret) {
      case Z_NEED_DICT:
        ret = Z_DATA_ERROR; /* and fall through */
      case Z_DATA_ERROR:
      case Z_MEM_ERROR:
        (void)inflateEnd(&strm);
        return ret;
      }
      have = CHUNK - strm.avail_out;
      if (!header_found) {
        for (size_t i = 0; i < have; ++i) {
          if (out[i] == '\0') {
            header_found = true;
            header_offset = i + 1;
            break;
          }
        }
        if (header_found) {
          if (header_offset < have) {
            if (fwrite(out + header_offset, 1, have - header_offset, stdout) !=
                    have - header_offset ||
                ferror(stdout)) {
              (void)inflateEnd(&strm);
              return Z_ERRNO;
            }
          }
        }
      } else {
        if (fwrite(out, 1, have, stdout) != have || ferror(stdout)) {
          (void)inflateEnd(&strm);
          return Z_ERRNO;
        }
      }
    } while (strm.avail_out == 0);

    /* done when inflate() says it's done */
  } while (ret != Z_STREAM_END);

  // cleanup
  (void)inflateEnd(&strm);

  return 0;
}

int hash_object(char *file_name) {
  EVP_MD *md = EVP_MD_fetch(NULL, "SHA1", NULL);
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  unsigned int len = 0;
  unsigned char *outdigest = NULL;
  int ret = 1;
  char *buffer[CHUNK];
  size_t bytes;

  if (!EVP_DigestInit(ctx, md))
    return -1;

  FILE *fp = fopen(file_name, "rb");
  if (fp == NULL)
    return -1;
  while ((bytes = fread(buffer, 1, sizeof(buffer), fp)) != 0) {
    if (EVP_DigestUpdate(ctx, buffer, bytes) != 1) {
      fprintf(stderr, "Error updating digest\n");
      fclose(fp);
      EVP_MD_CTX_free(ctx);
      EVP_MD_free(md);
      return -1;
    }
  }

  outdigest = (unsigned char *)OPENSSL_malloc(EVP_MD_get_size(md));
  if (!EVP_DigestFinal_ex(ctx, outdigest, &len)) {
    fprintf(stderr, "Error allocating outdigest");
    fclose(fp);
    EVP_MD_free(md);
    EVP_MD_CTX_free(ctx);
    return -1;
  }
  printf("len: %d outdigest: ", len);
  for (unsigned int i = 0; i < len; i++) {
    printf("%02x", outdigest[i]);
  }

  std::string hash = digestToString(outdigest, len);
  std::string dir = ".git/objects/";
  std::string hash_dir = dir + hash.substr(0, 2);
  std::string hash_file = hash_dir + "/" + hash.substr(2);

  if (!std::filesystem::exists(hash_dir)) {
    if (!std::filesystem::create_directories(hash_dir)) {
      fprintf(stderr, "Error creating directory %s\n", hash_dir.c_str());
      fclose(fp);
      EVP_MD_CTX_free(ctx);
      EVP_MD_free(md);
      return -1;
    }
  }

  std::ofstream new_file(hash_file, std::ios::binary);
  if (!new_file.is_open()) {
    fprintf(stderr, "Error creating file %s\n", hash_file.c_str());
    fclose(fp);
    EVP_MD_CTX_free(ctx);
    EVP_MD_free(md);
    return -1;
  }

  new_file << "blob " << len << "\0";
  new_file.write(reinterpret_cast<const char *>(outdigest), len);

  // clean up
  fclose(fp);
  EVP_MD_free(md);
  EVP_MD_CTX_free(ctx);
  return 0;
}

std::string digestToString(unsigned char *digest, unsigned int len) {
  std::stringstream ss;
  ss << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < len; i++) {
    ss << std::setw(2) << static_cast<unsigned int>(digest[i]);
  }
  return ss.str();
}
