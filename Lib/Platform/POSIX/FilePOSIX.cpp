#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Inline/I128.h"
#include "WAVM/Platform/File.h"
#include "WAVM/VFS/VFS.h"

using namespace WAVM;
using namespace WAVM::Platform;
using namespace WAVM::VFS;

static_assert(offsetof(struct iovec, iov_base) == offsetof(IOReadBuffer, data)
				  && offsetof(struct iovec, iov_len) == offsetof(IOReadBuffer, numBytes)
				  && sizeof(((struct iovec*)nullptr)->iov_base) == sizeof(IOReadBuffer::data)
				  && sizeof(((struct iovec*)nullptr)->iov_len) == sizeof(IOReadBuffer::numBytes)
				  && sizeof(struct iovec) == sizeof(IOReadBuffer),
			  "IOReadBuffer must match iovec");

static_assert(offsetof(struct iovec, iov_base) == offsetof(IOWriteBuffer, data)
				  && offsetof(struct iovec, iov_len) == offsetof(IOWriteBuffer, numBytes)
				  && sizeof(((struct iovec*)nullptr)->iov_base) == sizeof(IOWriteBuffer::data)
				  && sizeof(((struct iovec*)nullptr)->iov_len) == sizeof(IOWriteBuffer::numBytes)
				  && sizeof(struct iovec) == sizeof(IOWriteBuffer),
			  "IOWriteBuffer must match iovec");

static Result asVFSResult(int error)
{
	switch(error)
	{
	case ESPIPE: return Result::notSeekable;
	case EIO: return Result::ioDeviceError;
	case EINTR: return Result::interruptedBySignal;
	case EISDIR: return Result::isDirectory;
	case EFAULT: return Result::inaccessibleBuffer;
	case EFBIG: return Result::exceededFileSizeLimit;
	case EPERM: return Result::notPermitted;
	case EOVERFLOW: return Result::notEnoughBits;
	case EMFILE: return Result::outOfProcessFDs;
	case ENOTDIR: return Result::isNotDirectory;
	case EACCES: return Result::notAccessible;
	case EEXIST: return Result::alreadyExists;
	case ENAMETOOLONG: return Result::nameTooLong;
	case ENFILE: return Result::outOfSystemFDs;
	case ENOENT: return Result::doesNotExist;
	case ENOSPC: return Result::outOfFreeSpace;
	case EROFS: return Result::notPermitted;
	case ENOMEM: return Result::outOfMemory;
	case EDQUOT: return Result::outOfQuota;
	case ELOOP: return Result::tooManyLinksInPath;
	case EAGAIN: return Result::wouldBlock;
	case EINPROGRESS: return Result::ioPending;
	case ENOSR: return Result::outOfMemory;
	case ENXIO: return Result::missingDevice;
	case ETXTBSY: return Result::notAccessible;
	case EBUSY: return Result::busy;
	case ENOTEMPTY: return Result::isNotEmpty;
	case EMLINK: return Result::outOfLinksToParentDir;

	case EINVAL:
		// This probably needs to be handled differently for each API entry point.
		Errors::fatalfWithCallStack("ERROR_INVALID_PARAMETER");
	case EBADF: Errors::fatalfWithCallStack("EBADF");
	default: Errors::fatalfWithCallStack("Unexpected error code: %i (%s)", error, strerror(error));
	};
}

static FileType getFileTypeFromMode(mode_t mode)
{
	switch(mode & S_IFMT)
	{
	case S_IFBLK: return FileType::blockDevice;
	case S_IFCHR: return FileType::characterDevice;
	case S_IFIFO: return FileType::pipe;
	case S_IFREG: return FileType::file;
	case S_IFDIR: return FileType::directory;
	case S_IFLNK: return FileType::symbolicLink;
	case S_IFSOCK:
		// return FileType::datagramSocket;
		// return FileType::streamSocket;
		WAVM_UNREACHABLE();
		break;
	default: return FileType::unknown;
	};
}

static I128 timeToNS(time_t time)
{
	// time_t might be a long, and I128 doesn't have a long constructor, so coerce it to an integer
	// type first.
	const U64 timeInt = U64(time);
	return assumeNoOverflow(I128(timeInt) * 1000000000);
}

static void getFileInfoFromStatus(const struct stat& status, FileInfo& outInfo)
{
	outInfo.deviceNumber = status.st_dev;
	outInfo.fileNumber = status.st_ino;
	outInfo.type = getFileTypeFromMode(status.st_mode);
	outInfo.numLinks = status.st_nlink;
	outInfo.numBytes = status.st_size;
	outInfo.lastAccessTime = timeToNS(status.st_atime);
	outInfo.lastWriteTime = timeToNS(status.st_mtime);
	outInfo.creationTime = timeToNS(status.st_ctime);
}

static I32 translateFDFlags(const FDFlags& vfsFlags)
{
	I32 flags = 0;

	if(vfsFlags.append) { flags |= O_APPEND; }
	if(vfsFlags.nonBlocking) { flags |= O_NONBLOCK; }

	switch(vfsFlags.implicitSync)
	{
	case FDImplicitSync::none: break;
	case FDImplicitSync::syncContentsAfterWrite: flags |= O_DSYNC; break;
	case FDImplicitSync::syncContentsAndMetadataAfterWrite: flags |= O_SYNC; break;

#ifdef __APPLE__
		// Apple doesn't support O_RSYNC.
	case FDImplicitSync::syncContentsAfterWriteAndBeforeRead:
		Errors::fatal(
			"FDImplicitSync::syncContentsAfterWriteAndBeforeRead is not yet implemented on Apple "
			"platforms.");
	case FDImplicitSync::syncContentsAndMetadataAfterWriteAndBeforeRead:
		Errors::fatal(
			"FDImplicitSync::syncContentsAndMetadataAfterWriteAndBeforeRead is not yet implemented "
			"on Apple platforms.");
#else
	case FDImplicitSync::syncContentsAfterWriteAndBeforeRead: flags |= O_DSYNC | O_RSYNC; break;
	case FDImplicitSync::syncContentsAndMetadataAfterWriteAndBeforeRead:
		flags |= O_SYNC | O_RSYNC;
		break;
#endif

	default: WAVM_UNREACHABLE();
	}

	return flags;
}

struct POSIXDirEntStream : DirEntStream
{
	POSIXDirEntStream(DIR* inDir) : dir(inDir) {}

	virtual void close() override
	{
		closedir(dir);
		delete this;
	}

	virtual bool getNext(DirEnt& outEntry) override
	{
		errno = 0;
		struct dirent* dirent = readdir(dir);
		if(dirent)
		{
			wavmAssert(dirent);
			outEntry.fileNumber = dirent->d_ino;
			outEntry.name = dirent->d_name;
#ifdef _DIRENT_HAVE_D_TYPE
			switch(dirent->d_type)
			{
			case DT_BLK: outEntry.type = FileType::blockDevice; break;
			case DT_CHR: outEntry.type = FileType::characterDevice; break;
			case DT_DIR: outEntry.type = FileType::directory; break;
			case DT_FIFO: outEntry.type = FileType::pipe; break;
			case DT_LNK: outEntry.type = FileType::symbolicLink; break;
			case DT_REG: outEntry.type = FileType::file; break;
			case DT_SOCK: outEntry.type = FileType::streamSocket; break;
			case DT_UNKNOWN: outEntry.type = FileType::unknown; break;

			default: WAVM_UNREACHABLE();
			};
#else
			outEntry.type = FileType::unknown;
#endif
			return true;
		}
		else if(errno == 0)
		{
			// Reached the end of the directory.
			return false;
		}
		else if(errno == ENOENT || errno == EOVERFLOW)
		{
			return false;
		}
		else
		{
			Errors::fatalfWithCallStack("readdir returned unexpected error: %s", strerror(errno));
		}
	}

	virtual void restart() override
	{
		rewinddir(dir);
		maxValidOffset = 0;
	}

	virtual U64 tell() override
	{
		const long offset = telldir(dir);
		errorUnless(offset >= 0 && (unsigned long)offset <= UINT64_MAX);
		if(U64(offset) > maxValidOffset) { maxValidOffset = U64(offset); }
		return U64(offset);
	}

	virtual bool seek(U64 offset) override
	{
		// Don't allow seeking to higher offsets than have been returned by tell since the last
		// rewind.
		if(offset > maxValidOffset) { return false; };

		errorUnless(offset <= LONG_MAX);
		seekdir(dir, long(offset));
		return true;
	}

private:
	DIR* dir;
	U64 maxValidOffset{0};
};

struct POSIXFD : FD
{
	const I32 fd;

	POSIXFD(I32 inFD) : fd(inFD) {}

	virtual Result close() override
	{
		wavmAssert(fd >= 0);
		if(::close(fd))
		{
			// POSIX close says that the fd is in an undefined state after close returns EINTR.
			// This risks leaking the fd, but assume that the close completed despite the EINTR
			// error and return success.
			// https://www.daemonology.net/blog/2011-12-17-POSIX-close-is-broken.html
			if(errno != EINTR) { return asVFSResult(errno); }
		}
		delete this;
		return Result::success;
	}

	virtual Result seek(I64 offset, SeekOrigin origin, U64* outAbsoluteOffset = nullptr) override
	{
		I32 whence = 0;
		switch(origin)
		{
		case SeekOrigin::begin: whence = SEEK_SET; break;
		case SeekOrigin::cur: whence = SEEK_CUR; break;
		case SeekOrigin::end: whence = SEEK_END; break;
		default: WAVM_UNREACHABLE();
		};

#ifdef __linux__
		const I64 result = lseek64(fd, offset, whence);
#else
		const I64 result = lseek(fd, reinterpret_cast<off_t>(offset), whence);
#endif
		if(result == -1) { return errno == EINVAL ? Result::invalidOffset : asVFSResult(errno); }

		if(outAbsoluteOffset) { *outAbsoluteOffset = U64(result); }
		return Result::success;
	}
	virtual Result readv(const IOReadBuffer* buffers,
						 Uptr numBuffers,
						 Uptr* outNumBytesRead = nullptr) override
	{
		if(outNumBytesRead) { *outNumBytesRead = 0; }

		if(numBuffers == 0) { return Result::success; }
		else if(numBuffers > IOV_MAX)
		{
			return Result::tooManyBuffers;
		}

		// Do the read.
		ssize_t result = ::readv(fd, (const struct iovec*)buffers, numBuffers);
		if(result == -1) { return asVFSResult(errno); }

		if(outNumBytesRead) { *outNumBytesRead = result; }
		return Result::success;
	}
	virtual Result writev(const IOWriteBuffer* buffers,
						  Uptr numBuffers,
						  Uptr* outNumBytesWritten = nullptr) override
	{
		if(outNumBytesWritten) { *outNumBytesWritten = 0; }

		if(numBuffers == 0) { return Result::success; }
		else if(numBuffers > IOV_MAX)
		{
			return Result::tooManyBuffers;
		}

		ssize_t result = ::writev(fd, (const struct iovec*)buffers, numBuffers);
		if(result == -1) { return asVFSResult(errno); }

		if(outNumBytesWritten) { *outNumBytesWritten = result; }
		return Result::success;
	}
	virtual Result sync(SyncType syncType) override
	{
#ifdef __APPLE__
		I32 result = fsync(fd);
#else
		I32 result;
		switch(syncType)
		{
		case SyncType::contents: result = fdatasync(fd); break;
		case SyncType::contentsAndMetadata: result = fsync(fd); break;
		default: Errors::fatalfWithCallStack("Unexpected errno: %s", strerror(errno));
		}
#endif
		if(result) { return errno == EINVAL ? Result::notSynchronizable : asVFSResult(errno); }

		return Result::success;
	}
	virtual Result getFDInfo(FDInfo& outInfo) override
	{
		struct stat fdStatus;
		if(fstat(fd, &fdStatus) != 0) { return asVFSResult(errno); }

		outInfo.type = getFileTypeFromMode(fdStatus.st_mode);

		I32 fdFlags = fcntl(fd, F_GETFL);
		if(fdFlags < 0) { return asVFSResult(errno); }

		outInfo.flags.append = fdFlags & O_APPEND;
		outInfo.flags.nonBlocking = fdFlags & O_NONBLOCK;

		if(fdFlags & O_SYNC)
		{
#ifdef O_RSYNC
			outInfo.flags.implicitSync
				= fdFlags & O_RSYNC ? FDImplicitSync::syncContentsAndMetadataAfterWriteAndBeforeRead
									: FDImplicitSync::syncContentsAndMetadataAfterWrite;
#else
			outInfo.flags.implicitSync = FDImplicitSync::syncContentsAndMetadataAfterWrite;
#endif
		}
		else if(fdFlags & O_DSYNC)
		{
#ifdef O_RSYNC
			outInfo.flags.implicitSync = fdFlags & O_RSYNC
											 ? FDImplicitSync::syncContentsAfterWriteAndBeforeRead
											 : FDImplicitSync::syncContentsAfterWrite;
#else
			outInfo.flags.implicitSync = FDImplicitSync::syncContentsAfterWrite;
#endif
		}
		else
		{
			outInfo.flags.implicitSync = FDImplicitSync::none;
		}

		return Result::success;
	}

	virtual Result setFDFlags(const FDFlags& vfsFlags) override
	{
		const I32 flags = translateFDFlags(vfsFlags);
		return fcntl(fd, F_SETFL, flags) == 0 ? Result::success : asVFSResult(errno);
	}

	virtual Result getFileInfo(FileInfo& outInfo) override
	{
		struct stat fdStatus;

		if(fstat(fd, &fdStatus)) { return asVFSResult(errno); }

		getFileInfoFromStatus(fdStatus, outInfo);
		return Result::success;
	}

	virtual Result openDir(DirEntStream*& outStream) override
	{
		const I32 duplicateFD = dup(fd);
		if(duplicateFD < 0) { return asVFSResult(errno); }

		DIR* dir = fdopendir(duplicateFD);
		if(!dir) { return asVFSResult(errno); }

		// Rewind the dir to the beginning to ensure previous seeks on the FD don't affect the
		// dirent stream.
		rewinddir(dir);

		outStream = new POSIXDirEntStream(dir);
		return Result::success;
	}
};

struct POSIXStdFD : POSIXFD
{
	POSIXStdFD(I32 inFD) : POSIXFD(inFD) {}

	virtual Result close() override
	{
		// The stdio FDs are shared, so don't close them.
		return Result::success;
	}
};

FD* Platform::getStdFD(StdDevice device)
{
	static POSIXStdFD* stdinVFD = new POSIXStdFD(0);
	static POSIXStdFD* stdoutVFD = new POSIXStdFD(1);
	static POSIXStdFD* stderrVFD = new POSIXStdFD(2);
	switch(device)
	{
	case StdDevice::in: return stdinVFD; break;
	case StdDevice::out: return stdoutVFD; break;
	case StdDevice::err: return stderrVFD; break;
	default: WAVM_UNREACHABLE();
	};
}

Result Platform::openHostFile(const std::string& pathName,
							  FileAccessMode accessMode,
							  FileCreateMode createMode,
							  FD*& outFD,
							  const FDFlags& vfsFlags)
{
	U32 flags = 0;
	switch(accessMode)
	{
	case FileAccessMode::none: flags = O_RDONLY; break;
	case FileAccessMode::readOnly: flags = O_RDONLY; break;
	case FileAccessMode::writeOnly: flags = O_WRONLY; break;
	case FileAccessMode::readWrite: flags = O_RDWR; break;
	default: WAVM_UNREACHABLE();
	};

	switch(createMode)
	{
	case FileCreateMode::createAlways: flags |= O_CREAT | O_TRUNC; break;
	case FileCreateMode::createNew: flags |= O_CREAT | O_EXCL; break;
	case FileCreateMode::openAlways: flags |= O_CREAT; break;
	case FileCreateMode::openExisting: break;
	case FileCreateMode::truncateExisting: flags |= O_TRUNC; break;
	default: WAVM_UNREACHABLE();
	};

	mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;

	flags |= translateFDFlags(vfsFlags);

	const I32 fd = open(pathName.c_str(), flags, mode);
	if(fd == -1) { return asVFSResult(errno); }

	outFD = new POSIXFD(fd);
	return Result::success;
}

Result Platform::getHostFileInfo(const std::string& pathName, VFS::FileInfo& outInfo)
{
	struct stat fileStatus;

	if(stat(pathName.c_str(), &fileStatus)) { return asVFSResult(errno); }

	getFileInfoFromStatus(fileStatus, outInfo);
	return Result::success;
}

Result Platform::openHostDir(const std::string& pathName, DirEntStream*& outStream)
{
	DIR* dir = opendir(pathName.c_str());
	if(!dir) { return asVFSResult(errno); }

	outStream = new POSIXDirEntStream(dir);
	return Result::success;
}

Result Platform::unlinkHostFile(const std::string& pathName)
{
	return !unlink(pathName.c_str()) ? VFS::Result::success : asVFSResult(errno);
}

Result Platform::removeHostDir(const std::string& pathName)
{
	return !unlinkat(AT_FDCWD, pathName.c_str(), AT_REMOVEDIR) ? Result::success
															   : asVFSResult(errno);
}

Result Platform::createHostDir(const std::string& pathName)
{
	return !mkdir(pathName.c_str(), 0666) ? Result::success : asVFSResult(errno);
}

std::string Platform::getCurrentWorkingDirectory()
{
	const Uptr maxPathBytes = pathconf(".", _PC_PATH_MAX);
	char* buffer = (char*)alloca(maxPathBytes);
	errorUnless(getcwd(buffer, maxPathBytes) == buffer);
	return std::string(buffer);
}
