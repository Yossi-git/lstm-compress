#include <fstream>
#include <ctime>
#include <stdio.h>
#include <cstdlib>
#include <vector>
#include <valarray>

#include "preprocess/preprocessor.h"
#include "coder/encoder.h"
#include "coder/decoder.h"
#include "predictor.h"

namespace {
  const int kMinVocabFileSize = 10000;

  inline float Rand() {
    return static_cast <float> (rand()) / static_cast <float> (RAND_MAX);
  }
}

int Help() {
  printf("lstm-compress v3\n");
  printf("With preprocessing:\n");
  printf("    compress:           lstm-compress -c [dictionary] [input]"
      " [output]\n");
  printf("    only preprocessing: lstm-compress -s [dictionary] [input]"
      " [output]\n");
  printf("    decompress:         lstm-compress -d [dictionary] [input]"
      " [output]\n");
  printf("Without preprocessing:\n");
  printf("    compress:   lstm-compress -c [input] [output]\n");
  printf("    decompress: lstm-compress -d [input] [output]\n");
  printf("    generate:   lstm-compress -g [input] [output] [output size]\n");
  return -1;
}

void WriteHeader(unsigned long long length, const std::vector<bool>& vocab,
    std::ofstream* os) {
  for (int i = 4; i >= 0; --i) {
    char c = length >> (8*i);
    os->put(c);
  }
  if (length < kMinVocabFileSize) return;
  for (int i = 0; i < 32; ++i) {
    unsigned char c = 0;
    for (int j = 0; j < 8; ++j) {
      if (vocab[i * 8 + j]) c += 1<<j;
    }
    os->put(c);
  }
}

void WriteStorageHeader(FILE* out) {
  for (int i = 4; i >= 0; --i) {
    putc(0, out);
  }
}

void ReadHeader(std::ifstream* is, unsigned long long* length,
    std::vector<bool>* vocab) {
  *length = 0;
  for (int i = 0; i < 5; ++i) {
    *length <<= 8;
    *length += (unsigned char)(is->get());
  }
  if (*length == 0) return;
  if (*length < kMinVocabFileSize) {
    std::fill(vocab->begin(), vocab->end(), true);
    return;
  }
  for (int i = 0; i < 32; ++i) {
    unsigned char c = is->get();
    for (int j = 0; j < 8; ++j) {
      if (c & (1<<j)) (*vocab)[i * 8 + j] = true;
    }
  }
}

void ExtractVocab(unsigned long long input_bytes, std::ifstream* is,
    std::vector<bool>* vocab) {
  for (unsigned long long pos = 0; pos < input_bytes; ++pos) {
    unsigned char c = is->get();
    (*vocab)[c] = true;
  }
}

void Compress(unsigned long long input_bytes, std::ifstream* is,
    std::ofstream* os, unsigned long long* output_bytes, Predictor* p) {
  Encoder e(os, p);
  unsigned long long percent = 1 + (input_bytes / 100);
  for (unsigned long long pos = 0; pos < input_bytes; ++pos) {
    char c = is->get();
    for (int j = 7; j >= 0; --j) {
      e.Encode((c>>j)&1);
    }
    if (pos % percent == 0) {
      printf("\rprogress: %lld%%", pos / percent);
      fflush(stdout);
    }
  }
  e.Flush();
  *output_bytes = os->tellp();
}

void Decompress(unsigned long long output_length, std::ifstream* is,
                std::ofstream* os, const std::vector<bool>& vocab) {
  Predictor p(vocab);
  Decoder d(is, &p);
  unsigned long long percent = 1 + (output_length / 100);
  for(unsigned long long pos = 0; pos < output_length; ++pos) {
    int byte = 1;
    while (byte < 256) {
      byte += byte + d.Decode();
    }
    os->put(byte);
    if (pos % percent == 0) {
      printf("\rprogress: %lld%%", pos / percent);
      fflush(stdout);
    }
  }
}

bool Store(const std::string& input_path, const std::string& temp_path,
    const std::string& output_path, FILE* dictionary,
    unsigned long long* input_bytes, unsigned long long* output_bytes) {
  FILE* data_in = fopen(input_path.c_str(), "rb");
  if (!data_in) return false;
  FILE* data_out = fopen(output_path.c_str(), "wb");
  if (!data_out) return false;
  fseek(data_in, 0L, SEEK_END);
  *input_bytes = ftell(data_in);
  fseek(data_in, 0L, SEEK_SET);
  WriteStorageHeader(data_out);
  preprocessor::Encode(data_in, data_out, *input_bytes, temp_path, dictionary);
  fseek(data_out, 0L, SEEK_END);
  *output_bytes = ftell(data_out);
  fclose(data_in);
  fclose(data_out);
  return true;
}

bool RunCompression(bool enable_preprocess, const std::string& input_path,
    const std::string& temp_path, const std::string& output_path,
    FILE* dictionary, unsigned long long* input_bytes,
    unsigned long long* output_bytes) {
  FILE* data_in = fopen(input_path.c_str(), "rb");
  if (!data_in) return false;
  FILE* temp_out = fopen(temp_path.c_str(), "wb");
  if (!temp_out) return false;

  fseek(data_in, 0L, SEEK_END);
  *input_bytes = ftell(data_in);
  fseek(data_in, 0L, SEEK_SET);

  if (enable_preprocess) {
    preprocessor::Encode(data_in, temp_out, *input_bytes, temp_path,
        dictionary);
  } else {
    preprocessor::NoPreprocess(data_in, temp_out, *input_bytes);
  }
  fclose(data_in);
  fclose(temp_out);

  std::ifstream temp_in(temp_path, std::ios::in | std::ios::binary);
  if (!temp_in.is_open()) return false;

  std::ofstream data_out(output_path, std::ios::out | std::ios::binary);
  if (!data_out.is_open()) return false;

  temp_in.seekg(0, std::ios::end);
  unsigned long long temp_bytes = temp_in.tellg();
  temp_in.seekg(0, std::ios::beg);

  std::vector<bool> vocab(256, false);
  if (temp_bytes < kMinVocabFileSize) {
    std::fill(vocab.begin(), vocab.end(), true);
  } else {
    ExtractVocab(temp_bytes, &temp_in, &vocab);
    temp_in.seekg(0, std::ios::beg);
  }

  WriteHeader(temp_bytes, vocab, &data_out);
  Predictor p(vocab);
  Compress(temp_bytes, &temp_in, &data_out, output_bytes, &p);
  temp_in.close();
  data_out.close();
  remove(temp_path.c_str());
  return true;
}

bool RunGeneration(const std::string& input_path, const std::string& output_path,
    int output_size) {
  std::ifstream data_in(input_path, std::ios::in | std::ios::binary);
  if (!data_in.is_open()) return false;

  std::ofstream data_out(output_path, std::ios::out | std::ios::binary);
  if (!data_out.is_open()) return false;

  data_in.seekg(0, std::ios::end);
  unsigned long long input_bytes = data_in.tellg();
  data_in.seekg(0, std::ios::beg);
  std::vector<bool> vocab(256, false);
  ExtractVocab(input_bytes, &data_in, &vocab);
  data_in.seekg(0, std::ios::beg);

  int vocab_size = 0;
  for (int i = 0; i < 256; ++i) {
    if (vocab[i]) ++vocab_size;
  }
  int offset = 0;
  std::vector<int> byte_map(256, 0), reverse_byte_map(vocab_size, 0);
  for (int i = 0; i < 256; ++i) {
    byte_map[i] = offset;
    if (vocab[i]) ++offset;
  }
  offset = 0;
  for (int i = 0; i < 256; ++i) {
    if (vocab[i]) {
      reverse_byte_map[offset] = i;
      ++offset;
    }
  }

  Lstm lstm(vocab_size, vocab_size, 90, 3, 10, 0.05, 2);
  std::valarray<float>& probs = lstm.Perceive(byte_map[data_in.get()]);
  double entropy = log2(1.0/256);
  unsigned long long percent = 1 + (input_bytes / 100);
  for (unsigned int pos = 1; pos < input_bytes; ++pos) {
    int c = byte_map[data_in.get()];
    entropy += log2(probs[(unsigned char)c]);
    probs = lstm.Perceive(c);
    if (pos % percent == 0) {
      printf("\rtraining: %lld%%", pos / percent);
      fflush(stdout);
    }
  }
  entropy = -entropy / input_bytes;
  printf("\rcross entropy: %.4f\n", entropy);

  data_in.close();

  percent = 1 + (output_size / 100);
  for (int i = 0; i < output_size; ++i) {
    float r = Rand();
    int c = 0;
    for (; c < vocab_size; ++c) {
      r -= probs[c];
      if (r < 0) break;
    }
    probs = lstm.Predict(c);
    data_out.put(reverse_byte_map[c]);
    if (i % percent == 0) {
      printf("\rgeneration: %lld%%", i / percent);
      fflush(stdout);
    }
  }
  printf("\rgeneration: 100%%\n");  

  data_out.close();

  return true;
}

bool RunDecompression(bool enable_preprocess, const std::string& input_path,
    const std::string& temp_path, const std::string& output_path,
    FILE* dictionary, unsigned long long* input_bytes,
    unsigned long long* output_bytes) {
  std::ifstream data_in(input_path, std::ios::in | std::ios::binary);
  if (!data_in.is_open()) return false;

  data_in.seekg(0, std::ios::end);
  *input_bytes = data_in.tellg();
  data_in.seekg(0, std::ios::beg);
  std::vector<bool> vocab(256, false);
  ReadHeader(&data_in, output_bytes, &vocab);

  if (*output_bytes == 0) {  // undo store
    if (!enable_preprocess) return false;
    data_in.close();
    FILE* in = fopen(input_path.c_str(), "rb");
    if (!in) return false;
    FILE* data_out = fopen(output_path.c_str(), "wb");
    if (!data_out) return false;
    fseek(in, 5L, SEEK_SET);
    preprocessor::Decode(in, data_out, dictionary);
    fseek(data_out, 0L, SEEK_END);
    *output_bytes = ftell(data_out);
    fclose(in);
    fclose(data_out);
    return true;
  }

  std::ofstream temp_out(temp_path, std::ios::out | std::ios::binary);
  if (!temp_out.is_open()) return false;

  Decompress(*output_bytes, &data_in, &temp_out, vocab);
  data_in.close();
  temp_out.close();

  FILE* temp_in = fopen(temp_path.c_str(), "rb");
  if (!temp_in) return false;
  FILE* data_out = fopen(output_path.c_str(), "wb");
  if (!data_out) return false;

  preprocessor::Decode(temp_in, data_out, dictionary);
  fseek(data_out, 0L, SEEK_END);
  *output_bytes = ftell(data_out);
  fclose(temp_in);
  fclose(data_out);
  remove(temp_path.c_str());
  return true;
}

int main(int argc, char* argv[]) {
  if (argc < 4 || argc > 5 || argv[1][0] != '-' ||
      (argv[1][1] != 'c' && argv[1][1] != 'd' && argv[1][1] != 's' &&
      argv[1][1] != 'g')) {
    return Help();
  }

  clock_t start = clock();

  bool enable_preprocess = false;
  std::string input_path = argv[2];
  std::string output_path = argv[3];

  if (argv[1][1] == 'g') {
    if (argc != 5) return Help();
    int output_size = std::stoi(argv[4]);
    if (!RunGeneration(input_path, output_path, output_size)) {
      return Help();
    }
    return 0;
  }

  FILE* dictionary = NULL;
  if (argc == 5) {
    enable_preprocess = true;
    dictionary = fopen(argv[2], "rb");
    if (!dictionary) return Help();
    input_path = argv[3];
    output_path = argv[4];
  }

  std::string temp_path = output_path + ".lstm.temp";

  unsigned long long input_bytes = 0, output_bytes = 0;

  if (argv[1][1] == 's') {
    if (!enable_preprocess) return Help();
    if (!Store(input_path, temp_path, output_path, dictionary, &input_bytes,
        &output_bytes)) {
      return Help();
    }
  } else if (argv[1][1] == 'c') {
    if (!RunCompression(enable_preprocess, input_path, temp_path, output_path,
        dictionary, &input_bytes, &output_bytes)) {
      return Help();
    }
  } else {
    if (!RunDecompression(enable_preprocess, input_path, temp_path, output_path,
        dictionary, &input_bytes, &output_bytes)) {
      return Help();
    }
  }

  printf("\r%lld bytes -> %lld bytes in %1.2f s.\n",
      input_bytes, output_bytes,
      ((double)clock() - start) / CLOCKS_PER_SEC);

  if (argv[1][1] == 'c') {
    double cross_entropy = output_bytes;
    cross_entropy /= input_bytes;
    cross_entropy *= 8;
    printf("cross entropy: %.3f\n", cross_entropy);
  }

  return 0;
}
