#define NOMINMAX
#include "storm.h"
#include "../rmpq/archive.h"
#include "../trace.h"
#include "../ui/common.h"

#include <map>

BOOL  SFileOpenArchive(const char *szMpqName, DWORD dwPriority, DWORD dwFlags, HANDLE *phMpq) {
  File file(szMpqName, "rb");
  if (!file) {
    return FALSE;
  }
  mpq::Archive* archive = new mpq::Archive(file);
  *phMpq = (HANDLE) archive;
  return TRUE;
}

BOOL  SFileCloseArchive(HANDLE hArchive) {
  delete ((mpq::Archive*) hArchive);
  return TRUE;
}

BOOL  SFileCloseFile(HANDLE hFile) {
  delete ((File*) hFile);
  return TRUE;
}

BOOL  SFileGetFileArchive(HANDLE hFile, HANDLE *archive) {
  //MpqFile* file = (MpqFile*) hFile;
  *archive = NULL;
  return TRUE;
}

LONG  SFileGetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh) {
  File* file = (File*) hFile;
  uint64_t size = file->size();
  if (lpFileSizeHigh) {
    *lpFileSizeHigh = (DWORD) (size >> 32);
  }
  return (LONG) size;
}

BOOL  SFileOpenFile(const char *filename, HANDLE *phFile) {
  // Check the custom override archive first so that any file present in
  // unbound.mpq shadows the corresponding entry in diabdat.mpq.
  // Only fall back to the base archive when the file is absent from
  // unbound.mpq; a corrupt entry there is an error, not a silent bypass.
  if (unbound_mpq) {
    if (SFileOpenFileEx((HANDLE) unbound_mpq, filename, 0, phFile))
      return TRUE;
    if (SErrGetLastError() != ERROR_FILE_NOT_FOUND)
      return FALSE;
  }
  return SFileOpenFileEx((HANDLE) diabdat_mpq, filename, 0, phFile);
}

BOOL  SFileOpenFileEx(HANDLE hMpq, const char *szFileName, DWORD dwSearchScope, HANDLE *phFile) {
  mpq::Archive* archive = (mpq::Archive*) hMpq;
  auto pos = archive->findFile(szFileName);
  if (pos < 0) {
    SErrSetLastError(ERROR_FILE_NOT_FOUND);
    return FALSE;
  }
  auto file = archive->load_(pos, mpq::hashString(mpq::path_name(szFileName), mpq::HASH_KEY), true);
  if (file) {
    File* mpqf = new File(file);
    *phFile = (HANDLE) mpqf;
    return TRUE;
  }
  SErrSetLastError(ERROR_FILE_CORRUPT);
  return FALSE;
}

BOOL  SFileReadFile(HANDLE hFile, void *buffer, DWORD nNumberOfBytesToRead, DWORD *read, LONG *lpDistanceToMoveHigh) {
  File* file = (File*) hFile;
  auto res = file->read(buffer, nNumberOfBytesToRead);
  if (read) {
    *read = (DWORD) res;
  }
  return res != 0;
}

BOOL  SFileSetBasePath(char *) {
  return TRUE;
}

int  SFileSetFilePointer(HANDLE hFile, int pos, HANDLE, int origin) {
  File* file = (File*) hFile;
  file->seek(pos, origin);
  return 0;
}

BOOL  SFileGetFileSizeFast(const char* filename, LPDWORD size) {
  // Check unbound.mpq first so custom replacements report the correct size.
  if (unbound_mpq) {
    mpq::Archive* uarc = (mpq::Archive*) unbound_mpq;
    auto upos = uarc->findFile(filename);
    if (upos >= 0) {
      *size = uarc->getFileSize(upos);
      return TRUE;
    }
  }
  mpq::Archive* arc = (mpq::Archive*) diabdat_mpq;
  auto pos = arc->findFile(filename);
  if (pos >= 0) {
    *size = arc->getFileSize(pos);
    return TRUE;
  }
  return FALSE;
}
