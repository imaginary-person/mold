#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Timer.h"
#include "tbb/blocked_range.h"
#include "tbb/concurrent_hash_map.h"
#include "tbb/parallel_for_each.h"
#include "tbb/parallel_sort.h"
#include "tbb/partitioner.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>

using llvm::ArrayRef;
using llvm::ErrorOr;
using llvm::Error;
using llvm::Expected;
using llvm::MemoryBufferRef;
using llvm::SmallVector;
using llvm::StringRef;
using llvm::Twine;
using llvm::object::ELF64LE;
using llvm::object::ELFFile;

class Symbol;
class ObjectFile;
class InputSection;

struct Config {
  StringRef output;
};

extern Config config;

[[noreturn]] inline void error(const Twine &msg) {
  llvm::errs() << msg << "\n";
  exit(1);
}

template <class T> T check(ErrorOr<T> e) {
  if (auto ec = e.getError())
    error(ec.message());
  return std::move(*e);
}

template <class T> T check(Expected<T> e) {
  if (!e)
    error(llvm::toString(e.takeError()));
  return std::move(*e);
}

template <class T>
T check2(ErrorOr<T> e, llvm::function_ref<std::string()> prefix) {
  if (auto ec = e.getError())
    error(prefix() + ": " + ec.message());
  return std::move(*e);
}

template <class T>
T check2(Expected<T> e, llvm::function_ref<std::string()> prefix) {
  if (!e)
    error(prefix() + ": " + toString(e.takeError()));
  return std::move(*e);
}

inline std::string toString(const Twine &s) { return s.str(); }

#define CHECK(E, S) check2((E), [&] { return toString(S); })

class Symbol;
class SymbolTable;
class InputSection;
class ObjectFile;

//
// intern.cc
//

class InternedString {
public:
  InternedString() {}
  InternedString(const InternedString &other) = default;

  InternedString(const char *data_, size_t size_)
    : data_(data_), size_(size_) {}

  explicit InternedString(StringRef s);
  explicit operator StringRef() const { return {data_, size_}; }
  
  const char *data() { return data_; }
  uint32_t size() { return size_; }

private:
  const char *data_ = nullptr;
  uint32_t size_ = 0;
};

inline InternedString intern(StringRef s) {
  return InternedString(s);
}

//
// symtab.cc
//

class Symbol {
public:
  InternedString name;
  ObjectFile *file;
};

class SymbolTable {
public:
  Symbol *add(InternedString key, Symbol sym);
  Symbol *get(InternedString key);

private:
  typedef tbb::concurrent_hash_map<uintptr_t, Symbol> MapType;

  MapType map;
};

//
// input_sections.cc
//

class InputChunk {
public:
  virtual void copy_to(uint8_t *buf) = 0;
  virtual void relocate(uint8_t *buf) {}

  StringRef name;
};

class InputSection : public InputChunk {
public:
  InputSection(ObjectFile *file, const ELF64LE::Shdr *hdr, StringRef name);
  void copy_to(uint8_t *buf);
  uint64_t get_size() const;

  uint64_t output_file_offset;
  int64_t offset = -1;

private:
  const ELF64LE::Shdr *hdr;
  ObjectFile *file;
};

class StringTableSection : public InputChunk {
public:
  StringTableSection(StringRef name) {
    this->name = name;
  }

  uint64_t addString(StringRef s);
  void copy_to(uint8_t *buf) override;
};

//
// output_sections.cc
//

class OutputChunk {
public:
  virtual ~OutputChunk() {}

  virtual void copy_to(uint8_t *buf) = 0;
  virtual void relocate(uint8_t *buf) = 0;
  virtual void set_offset(uint64_t off) { offset = off; }
  uint64_t get_offset() const { return offset; }
  virtual uint64_t get_size() const = 0;

protected:
  int64_t offset = -1;
  int64_t size = -1;
};

// ELF header
class OutputEhdr : public OutputChunk {
public:
  void copy_to(uint8_t *buf) override {}
  void relocate(uint8_t *buf) override;

  uint64_t get_size() const override {
    return sizeof(ELF64LE::Ehdr);
  }
};

// Section header
class OutputShdr : public OutputChunk {
public:
  void copy_to(uint8_t *buf) override {
    memcpy(buf + offset, &hdr[0], get_size());
  }

  void relocate(uint8_t *buf) override {}

  uint64_t get_size() const override {
    return hdr.size() * sizeof(hdr[0]);
  }

  std::vector<ELF64LE::Shdr> hdr;
};

// Program header
class OutputPhdr : public OutputChunk {
public:
  void copy_to(uint8_t *buf) override {
    memcpy(buf + offset, &hdr[0], get_size());
  }

  void relocate(uint8_t *buf) override {}

  uint64_t get_size() const override {
    return hdr.size() * sizeof(hdr[0]);
  }

  std::vector<ELF64LE::Phdr> hdr;
};

// Sections
class OutputSection : public OutputChunk {
public:
  OutputSection(StringRef name) : name(name) {}

  void copy_to(uint8_t *buf) override {
    for (InputSection *sec : sections)
      sec->copy_to(buf);
  }

  void relocate(uint8_t *buf) override {}

  uint64_t get_size() const override {
    assert(size >= 0);
    return size;
  }

  void set_offset(uint64_t off) override;

  std::vector<InputSection *> sections;
  StringRef name;

private:
  uint64_t file_offset = 0;
  uint64_t on_file_size = -1;
};

namespace out {
extern OutputEhdr *ehdr;
extern OutputShdr *shdr;
extern OutputPhdr *phdr;
}

//
// input_files.cc
//

class ObjectFile { 
public:
  ObjectFile(MemoryBufferRef mb, StringRef archive_name);

  void parse();
  void register_defined_symbols();
  void register_undefined_symbols();
  StringRef get_filename();

  std::vector<InputSection *> sections;
  StringRef archive_name;
  int priority;
  bool is_alive = false;
  std::unordered_set<ObjectFile *> liveness_edges;
  ELFFile<ELF64LE> obj;

private:
  MemoryBufferRef mb;
  std::vector<Symbol *> symbols;
  std::vector<Symbol> symbol_instances;
  ArrayRef<ELF64LE::Sym> elf_syms;
  int first_global = 0;
};

//
// writer.cc
//

void write();

//
// output_file.cc
//

class OutputFile {
public:
  OutputFile(uint64_t size);
  void commit();

private:
  std::unique_ptr<llvm::FileOutputBuffer> output_buffer;
  uint8_t *buf;
};

//
// main.cc
//

MemoryBufferRef readFile(StringRef path);

std::string toString(ObjectFile *);

extern SymbolTable symbol_table;
extern std::atomic_int num_defined;
extern std::atomic_int num_undefined;
extern std::atomic_int num_files;