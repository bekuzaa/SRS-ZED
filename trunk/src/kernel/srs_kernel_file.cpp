//
// Copyright (c) 2013-2025 The SRS Authors
//
// SPDX-License-Identifier: MIT
//

#include <srs_kernel_file.hpp>

// for srs-librtmp, @see https://github.com/ossrs/srs/issues/213
#include <sys/uio.h>
#include <unistd.h>

#include <fcntl.h>
#include <sstream>
using namespace std;

#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>

// For utest to mock it.
srs_open_t _srs_open_fn = ::open;
srs_write_t _srs_write_fn = ::write;
srs_read_t _srs_read_fn = ::read;
srs_lseek_t _srs_lseek_fn = ::lseek;
srs_close_t _srs_close_fn = ::close;

srs_fopen_t _srs_fopen_fn = ::fopen;
srs_fwrite_t _srs_fwrite_fn = ::fwrite;
srs_fread_t _srs_fread_fn = ::fread;
srs_fseek_t _srs_fseek_fn = ::fseek;
srs_fclose_t _srs_fclose_fn = ::fclose;
srs_ftell_t _srs_ftell_fn = ::ftell;
srs_setvbuf_t _srs_setvbuf_fn = ::setvbuf;

ISrsFileWriter::ISrsFileWriter()
{
}

ISrsFileWriter::~ISrsFileWriter()
{
}

SrsFileWriter::SrsFileWriter()
{
    fp_ = NULL;
    buf_ = NULL;
}

SrsFileWriter::~SrsFileWriter()
{
    close();
    srs_freepa(buf_);
}

srs_error_t SrsFileWriter::set_iobuf_size(int size)
{
    srs_error_t err = srs_success;

    if (fp_ == NULL) {
        return srs_error_new(ERROR_SYSTEM_FILE_NOT_OPEN, "file %s is not opened", path_.c_str());
    }

    srs_freepa(buf_);
    buf_ = size > 0 ? new char[size] : NULL;
    int r0 = _srs_setvbuf_fn(fp_, buf_, _IOFBF, size);
    if (r0) {
        return srs_error_new(ERROR_SYSTEM_FILE_SETVBUF, "setvbuf err, file=%s, r0=%d", path_.c_str(), r0);
    }

    return err;
}

srs_error_t SrsFileWriter::open(string p)
{
    srs_error_t err = srs_success;

    if (fp_ != NULL) {
        return srs_error_new(ERROR_SYSTEM_FILE_ALREADY_OPENED, "file %s already opened", p.c_str());
    }

    if ((fp_ = _srs_fopen_fn(p.c_str(), "wb")) == NULL) {
        return srs_error_new(ERROR_SYSTEM_FILE_OPENE, "open file %s failed", p.c_str());
    }

    path_ = p;

    return err;
}

srs_error_t SrsFileWriter::open_append(string p)
{
    srs_error_t err = srs_success;

    if (fp_ != NULL) {
        return srs_error_new(ERROR_SYSTEM_FILE_ALREADY_OPENED, "file %s already opened", p.c_str());
    }

    if ((fp_ = _srs_fopen_fn(p.c_str(), "ab")) == NULL) {
        return srs_error_new(ERROR_SYSTEM_FILE_OPENE, "open file %s failed", p.c_str());
    }

    path_ = p;

    return err;
}

void SrsFileWriter::close()
{
    if (fp_ == NULL) {
        return;
    }

    if (_srs_fclose_fn(fp_) < 0) {
        srs_warn("close file %s failed", path_.c_str());
    }
    fp_ = NULL;

    return;
}

bool SrsFileWriter::is_open()
{
    return fp_ != NULL;
}

void SrsFileWriter::seek2(int64_t offset)
{
    srs_assert(is_open());

    int r0 = _srs_fseek_fn(fp_, (long)offset, SEEK_SET);
    srs_assert(r0 != -1);
}

int64_t SrsFileWriter::tellg()
{
    srs_assert(is_open());

    return _srs_ftell_fn(fp_);
}

srs_error_t SrsFileWriter::write(void *buf, size_t count, ssize_t *pnwrite)
{
    srs_error_t err = srs_success;

    if (fp_ == NULL) {
        return srs_error_new(ERROR_SYSTEM_FILE_NOT_OPEN, "file %s is not opened", path_.c_str());
    }

    size_t n = _srs_fwrite_fn(buf, 1, count, fp_);
    if (n != count) {
        return srs_error_new(ERROR_SYSTEM_FILE_WRITE, "write to file %s failed", path_.c_str());
    }

    if (pnwrite != NULL) {
        *pnwrite = (ssize_t)n;
    }

    return err;
}

srs_error_t SrsFileWriter::writev(const iovec *iov, int iovcnt, ssize_t *pnwrite)
{
    srs_error_t err = srs_success;

    ssize_t nwrite = 0;
    for (int i = 0; i < iovcnt; i++) {
        const iovec *piov = iov + i;
        ssize_t this_nwrite = 0;
        if ((err = write(piov->iov_base, piov->iov_len, &this_nwrite)) != srs_success) {
            return srs_error_wrap(err, "writev");
        }
        nwrite += this_nwrite;
    }

    if (pnwrite) {
        *pnwrite = nwrite;
    }

    return err;
}

srs_error_t SrsFileWriter::lseek(off_t offset, int whence, off_t *seeked)
{
    srs_assert(is_open());

    if (_srs_fseek_fn(fp_, (long)offset, whence) == -1) {
        return srs_error_new(ERROR_SYSTEM_FILE_SEEK, "seek file");
    }

    if (seeked) {
        *seeked = _srs_ftell_fn(fp_);
    }

    return srs_success;
}

ISrsFileReaderFactory::ISrsFileReaderFactory()
{
}

ISrsFileReaderFactory::~ISrsFileReaderFactory()
{
}

SrsFileReader *ISrsFileReaderFactory::create_file_reader()
{
    return new SrsFileReader();
}

ISrsFileReader::ISrsFileReader()
{
}

ISrsFileReader::~ISrsFileReader()
{
}

SrsFileReader::SrsFileReader()
{
    fd_ = -1;
}

SrsFileReader::~SrsFileReader()
{
    close();
}

srs_error_t SrsFileReader::open(string p)
{
    srs_error_t err = srs_success;

    if (fd_ > 0) {
        return srs_error_new(ERROR_SYSTEM_FILE_ALREADY_OPENED, "file %s already opened", path_.c_str());
    }

    if ((fd_ = _srs_open_fn(p.c_str(), O_RDONLY)) < 0) {
        return srs_error_new(ERROR_SYSTEM_FILE_OPENE, "open file %s failed", p.c_str());
    }

    path_ = p;

    return err;
}

void SrsFileReader::close()
{
    int ret = ERROR_SUCCESS;

    if (fd_ < 0) {
        return;
    }

    if (_srs_close_fn(fd_) < 0) {
        srs_warn("close file %s failed. ret=%d", path_.c_str(), ret);
    }
    fd_ = -1;

    return;
}

bool SrsFileReader::is_open()
{
    return fd_ > 0;
}

int64_t SrsFileReader::tellg()
{
    return (int64_t)_srs_lseek_fn(fd_, 0, SEEK_CUR);
}

void SrsFileReader::skip(int64_t size)
{
    off_t r0 = _srs_lseek_fn(fd_, (off_t)size, SEEK_CUR);
    srs_assert(r0 != -1);
}

int64_t SrsFileReader::seek2(int64_t offset)
{
    return (int64_t)_srs_lseek_fn(fd_, (off_t)offset, SEEK_SET);
}

int64_t SrsFileReader::filesize()
{
    int64_t cur = tellg();
    int64_t size = (int64_t)_srs_lseek_fn(fd_, 0, SEEK_END);

    off_t r0 = _srs_lseek_fn(fd_, (off_t)cur, SEEK_SET);
    srs_assert(r0 != -1);

    return size;
}

srs_error_t SrsFileReader::read(void *buf, size_t count, ssize_t *pnread)
{
    srs_error_t err = srs_success;

    ssize_t nread;
    // TODO: FIXME: use st_read.
    if ((nread = _srs_read_fn(fd_, buf, count)) < 0) {
        return srs_error_new(ERROR_SYSTEM_FILE_READ, "read from file %s failed", path_.c_str());
    }

    if (nread == 0) {
        return srs_error_new(ERROR_SYSTEM_FILE_EOF, "file EOF");
    }

    if (pnread != NULL) {
        *pnread = nread;
    }

    return err;
}

srs_error_t SrsFileReader::lseek(off_t offset, int whence, off_t *seeked)
{
    off_t sk = _srs_lseek_fn(fd_, offset, whence);
    if (sk < 0) {
        return srs_error_new(ERROR_SYSTEM_FILE_SEEK, "seek %d failed", (int)sk);
    }

    if (seeked) {
        *seeked = sk;
    }

    return srs_success;
}
