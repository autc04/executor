#pragma once


namespace Executor::mpw
{
	// from MPW's FCntl.h
	enum
	{
		kF_OPEN = (('d' << 8)| 0x00),
		kF_DELETE = (('d' << 8) | 0x01),
		kF_RENAME = (('d' << 8) | 0x02),
	};

	enum
	{
		kF_GTABINFO = (('e' << 8) | 0x00),		/* get tab offset for file */
		kF_STABINFO = (('e' << 8) | 0x01),		/* set 	"	"		"	"  */
		kF_GFONTINFO = (('e' << 8) | 0x02),		/* get font number and size for file */
		kF_SFONTINFO = (('e' << 8) | 0x03),		/* set 	"		"	"	"	"	" 	 */
		kF_GPRINTREC = (('e' << 8) | 0x04),		/* get print record for file */
		kF_SPRINTREC = (('e' << 8) | 0x05),		/* set 	"		"	"	" 	 */
		kF_GSELINFO = (('e' << 8) | 0x06),		/* get selection information for file */
		kF_SSELINFO = (('e' << 8) | 0x07),		/* set		"		"		"		" */
		kF_GWININFO = (('e' << 8) | 0x08),		/* get current window position */
		kF_SWININFO = (('e' << 8) | 0x09),		/* set	"		"		" 	   */
		kF_GSCROLLINFO = (('e' << 8) | 0x0A),	/* get scroll information */
		kF_SSCROLLINFO = (('e' << 8) | 0x0B),	/* set    "   		"  	  */
		kF_GMARKER = (('e' << 8) | 0x0D),		/* Get Marker */
		kF_SMARKER = (('e' << 8) | 0x0C),		/* Set   " 	  */
		kF_GSAVEONCLOSE = (('e' << 8) | 0x0F),	/* Get Save on close */
		kF_SSAVEONCLOSE = (('e' << 8) | 0x0E),	/* Set   "	 "	 " 	 */
	};


	// from MPW's IOCtl.h
	enum
	{
		kFIOLSEEK = (('f' << 8) | 0x00),		/* Apple internal use only */
		kFIODUPFD = (('f' << 8) | 0x01),		/* Apple internal use only */
		kFIOINTERACTIVE = (('f' << 8) | 0x02),	/* If device is interactive */
		kFIOBUFSIZE = (('f' << 8) | 0x03),		/* Return optimal buffer size */
		kFIOFNAME = (('f' << 8) | 0x04),		/* Return filename */
		kFIOREFNUM = (('f' << 8) | 0x05),		/* Return fs refnum */
		kFIOSETEOF = (('f' << 8) | 0x06),		/* Set file length */
	};

	enum
	{
		kSEEK_CUR = 1,
		kSEEK_END = 2,
		kSEEK_SET = 0,
	};

	enum
	{
		// despite these constants, 0x01 seems to be read, 0x02 is write, 0x03 is read/write.
		kO_RDONLY = 0,			/* Bits 0 and 1 are used internally */
		kO_WRONLY = 1, 			/* Values 0..2 are historical */
		kO_RDWR = 2,			/* NOTE: it goes 0, 1, 2, *!* 8, 16, 32, ... */
		kO_APPEND = (1 << 3),	/* append (writes guaranteed at the end) */
		kO_RSRC = (1 << 4),		/* Open the resource fork */
		kO_CREAT = (1 << 8),	/* Open with file create */
		kO_TRUNC = (1 << 9),	/* Open with truncation */
		kO_EXCL = (1 << 10), 	/* w/ O_CREAT Exclusive "create-only" */
		kO_BINARY = (1 << 11), 	/* Open as a binary stream */
	};


	// from MPW errno.h
	enum mpw_errno {
		kEPERM = 1,		/* Permission denied */
		kENOENT = 2,	/* No such file or directory */
		kENORSRC = 3,	/* No such resource */
		kEINTR = 4,		/* Interrupted system service */
		kEIO = 	5,		/* I/O error */
		kENXIO =  6,	/* No such device or address */
		kE2BIG =  7,	/* Argument list too long */
		kENOEXEC =  8,	/* Exec format error */
		kEBADF =  9,	/* Bad file number */
		kECHILD = 10,	/* No children processes */
		kEAGAIN = 11,	/* Resource temporarily unavailable, try again later */
		kENOMEM = 12,	/* Not enough space */
		kEACCES = 13,	/* Permission denied */
		kEFAULT = 14,	/* Bad address */
		kENOTBLK = 15,	/* Block device required */
		kEBUSY = 16,	/* Device or resource busy */
		kEEXIST = 17,	/* File exists */
		kEXDEV = 18,	/* Cross-device link */
		kENODEV = 19,	/* No such device */
		kENOTDIR = 20,	/* Not a directory */
		kEISDIR = 21,	/* Is a directory */
		kEINVAL = 22,	/* Invalid argument */
		kENFILE = 23,	/* File table overflow */
		kEMFILE = 24,	/* Too many open files */
		kENOTTY = 25,	/* Not a character device */
		kETXTBSY = 26,	/* Text file busy */
		kEFBIG = 27,	/* File too large */
		kENOSPC = 28,	/* No space left on device */
		kESPIPE = 29,	/* Illegal seek */
		kEROFS = 30,	/* Read only file system */
		kEMLINK = 31,	/* Too many links */
		kEPIPE = 32,	/* Broken pipe */
		kEDOM = 33,		/* Math arg out of domain of func */
		kERANGE = 34,	/* Math result not representable */
	};

    void SetupTool();
}
