/*
 * pg_wal_fp_extract
 *
 * Extract Full Page(s) from WAL.
 *
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *
 */

#define FRONTEND 1
#include "postgres.h"
#if PG_VERSION_NUM >= 120000
#include "common/logging.h"
#endif
#include "storage/fd.h"
#include "access/xlog_internal.h"
#include "common/fe_memutils.h"
#include "getopt_long.h"
#include "catalog/pg_control.h"
#include "common/pg_lzcompress.h"
#include "storage/checksum.h"
#include "storage/checksum_impl.h"

#if PG_VERSION_NUM >= 120000
#define fatal_error(...) do { pg_log_fatal(__VA_ARGS__); exit(EXIT_FAILURE); } while(0)
#else
static void fatal_error(const char *fmt,...) pg_attribute_printf(1, 2);
#endif

static int open_file_in_directory(const char *directory, const char *fname);
static const char *progname;

#if PG_VERSION_NUM >= 110000
static int      WalSegSz;
#endif

#if PG_VERSION_NUM < 120000
#include "access/transam.h"

static void
fatal_error(const char *fmt,...)
{
	va_list         args;

	fflush(stdout);

	fprintf(stderr, _("%s: FATAL:  "), progname);
	va_start(args, fmt);
	vfprintf(stderr, _(fmt), args);
	va_end(args);
	fputc('\n', stderr);

	exit(EXIT_FAILURE);
}
#endif

typedef struct XLogDumpPrivate
{
	TimeLineID      timeline;
	char       *inpath;
	XLogRecPtr      startptr;
	XLogRecPtr      endptr;
	bool            endptr_reached;
} XLogDumpPrivate;

typedef struct XLogDumpConfig
{
	bool            	checksum;
	bool            	dryrun;
	int                     filter_by_relid;
	TransactionId 		filter_by_xid;
	bool            	filter_by_xid_enabled;
	bool            	filter_by_relid_enabled;
	char			*dump_dest;
} XLogDumpConfig;

static void pgwfe_XLogDumpRecord(XLogDumpConfig *config, XLogReaderState *record);
static bool pgwfe_RestoreBlockImage(XLogDumpConfig *config, XLogReaderState *record,
				uint8 block_id, Oid spcNode, Oid dbNode, Oid relNode, const char *forkname, int blk);

static bool
pgwfe_RestoreBlockImage(XLogDumpConfig *config, XLogReaderState *record,
					uint8 block_id, Oid spcNode, Oid dbNode, Oid relNode, const char *forkname, int blk)
{
	DecodedBkpBlock *bkpb;
	char       *ptr;
	PGAlignedBlock tmp;
	char   *page;
	FILE *file;
	char fpath[MAXPGPATH];

	snprintf(fpath, MAXPGPATH, "%s/tbs_%u_db_%u_rel_%u_fork_%s_blk_%u_lsn_%X_%08X.dump",
						config->dump_dest, spcNode, dbNode, relNode, forkname, blk,
						(uint32) (record->ReadRecPtr >> 32),
						(uint32) record->ReadRecPtr);

	page = malloc(BLCKSZ);

	if (!record->blocks[block_id].in_use)
		return false;
	if (!record->blocks[block_id].has_image)
		return false;

	bkpb = &record->blocks[block_id];
	ptr = bkpb->bkp_image;

	if (bkpb->bimg_info & BKPIMAGE_IS_COMPRESSED)
	{
		printf("Dumping (compressed) FPI from rel %u/%u/%u fork %s blk %u from lsn %X/%08X to %s\n",
						spcNode, dbNode, relNode,
						forkname,
						blk,
						(uint32) (record->ReadRecPtr >> 32), (uint32) record->ReadRecPtr,
						fpath);

		/* If a backup block image is compressed, decompress it */
		if (pglz_decompress(ptr, bkpb->bimg_len, tmp.data,
#if PG_VERSION_NUM >= 120000
					BLCKSZ - bkpb->hole_length, true) < 0)
#else
					BLCKSZ - bkpb->hole_length) < 0)
#endif
		{
			printf("invalid compressed image at %X/%X, block %d",
					(uint32) (record->ReadRecPtr >> 32),
					(uint32) record->ReadRecPtr,
					block_id);
			return false;
		}
		ptr = tmp.data;
	} else {
		printf("Dumping (non-compressed) FPI from rel %u/%u/%u fork %s blk %u from lsn %X/%08X to %s\n",
						spcNode, dbNode, relNode,
						forkname,
						blk,
						(uint32) (record->ReadRecPtr >> 32), (uint32) record->ReadRecPtr,
						fpath);
	}

	if(!(config->dryrun))
	{
		file = fopen(fpath,"wb");
		/* generate page, taking into account hole if necessary */
		if (bkpb->hole_length == 0)
		{
			memcpy(page, ptr, BLCKSZ);
		}
		else
		{
			memcpy(page, ptr, bkpb->hole_offset);
			/* must zero-fill the hole */
			MemSet(page + bkpb->hole_offset, 0, bkpb->hole_length);
			memcpy(page + (bkpb->hole_offset + bkpb->hole_length),
						ptr + bkpb->hole_offset,
						BLCKSZ - (bkpb->hole_offset + bkpb->hole_length));
		}
		PageSetLSN(page, record->EndRecPtr);
		if((config->checksum))
			((PageHeader) page)->pd_checksum = pg_checksum_page(page, block_id);
		fwrite(page, BLCKSZ, 1, file);
		fclose(file);
	}
	free(page);
	return true;
}

static void
pgwfe_XLogDumpRecord(XLogDumpConfig *config, XLogReaderState *record)
{
	RelFileNode rnode;
	ForkNumber	forknum;
	BlockNumber blk;
	int			block_id;

	for (block_id = 0; block_id <= record->max_block_id; block_id++)
	{
		if (!XLogRecHasBlockRef(record, block_id))
			continue;

		XLogRecGetBlockTag(record, block_id, &rnode, &forknum, &blk);
		/* apply all specified filters */
		if (config->filter_by_relid_enabled &&
			config->filter_by_relid != rnode.relNode)
			continue;

		if (XLogRecHasBlockImage(record, block_id))
			if(!pgwfe_RestoreBlockImage(config,record,block_id,rnode.spcNode,
						rnode.dbNode,rnode.relNode,forkNames[forknum],blk))
				printf("failed to restore block image from rel %u/%u/%u fork %s blk %u from lsn %X/%08X\n",
						rnode.spcNode, rnode.dbNode, rnode.relNode,
						forkNames[forknum],
						blk,
						(uint32) (record->ReadRecPtr >> 32), (uint32) record->ReadRecPtr);
	}
}

/*
 * Read count bytes from a segment file in the specified directory, for the
 * given timeline, containing the specified record pointer; store the data in
 * the passed buffer.
 */
static void
XLogDumpXLogRead(const char *directory, TimeLineID timeline_id,
					XLogRecPtr startptr, char *buf, Size count)
{
	char       *p;
	XLogRecPtr      recptr;
	Size            nbytes;

	static int      file = -1;
	static XLogSegNo sendSegNo = 0;
	static uint32 sendOff = 0;

	p = buf;
	recptr = startptr;
	nbytes = count;

	while (nbytes > 0)
	{
		uint32          startoff;
		int                     segbytes;
		int                     readbytes;
#if PG_VERSION_NUM >= 110000
		startoff = XLogSegmentOffset(recptr, WalSegSz);
		if (!XLByteInSeg(recptr, sendSegNo, WalSegSz))
		{
			char            fname[MAXFNAMELEN];
			XLByteToSeg(recptr, sendSegNo, WalSegSz);
			XLogFileName(fname, timeline_id, sendSegNo, WalSegSz);
#else
		startoff = recptr % XLogSegSize;
		if (!XLByteInSeg(recptr, sendSegNo))
		{
			char            fname[MAXFNAMELEN];
			XLByteToSeg(recptr, sendSegNo);
			XLogFileName(fname, timeline_id, sendSegNo);
#endif
			file = open_file_in_directory(directory, fname);

			if (file < 0)
				fatal_error("could not find file \"%s\": %s",
				fname, strerror(errno));
			sendOff = 0;
		}

		/* Need to seek in the file? */
		if (sendOff != startoff)
		{
			if (lseek(file, (off_t) startoff, SEEK_SET) < 0)
			{
				int                     err = errno;
				char            fname[MAXPGPATH];
#if PG_VERSION_NUM >= 110000
				XLogFileName(fname, timeline_id, sendSegNo, WalSegSz);
#else
				XLogFileName(fname, timeline_id, sendSegNo);
#endif

				fatal_error("could not seek in log file %s to offset %u: %s",
						fname, startoff, strerror(err));
			}
			sendOff = startoff;
		}
#if PG_VERSION_NUM >= 110000
		/* How many bytes are within this segment? */
		if (nbytes > (WalSegSz - startoff))
			segbytes = WalSegSz - startoff;
#else
		if (nbytes > (XLogSegSize - startoff))
			segbytes = XLogSegSize - startoff;
#endif
		else
			segbytes = nbytes;

		readbytes = read(file, p, segbytes);
		if (readbytes <= 0)
		{
			int                     err = errno;
			char            fname[MAXPGPATH];
			int                     save_errno = errno;
#if PG_VERSION_NUM >= 110000
			XLogFileName(fname, timeline_id, sendSegNo, WalSegSz);
#else
			XLogFileName(fname, timeline_id, sendSegNo);
#endif
			errno = save_errno;

			if (readbytes < 0)
					fatal_error("could not read from log file %s, offset %u, length %d: %s",
							fname, sendOff, segbytes, strerror(err));
			else if (readbytes == 0)
				fatal_error("could not read from log file %s, offset %u: read %d of %zu",
							fname, sendOff, readbytes, (Size) segbytes);
		}
		/* Update state for read */
		recptr += readbytes;

		sendOff += readbytes;
		nbytes -= readbytes;
		p += readbytes;
	}
}

static int
XLogDumpReadPage(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
				 XLogRecPtr targetPtr, char *readBuff, TimeLineID *curFileTLI)
{
	XLogDumpPrivate *private = state->private_data;
	int                     count = XLOG_BLCKSZ;

	if (private->endptr != InvalidXLogRecPtr)
	{
		if (targetPagePtr + XLOG_BLCKSZ <= private->endptr)
			count = XLOG_BLCKSZ;
		else if (targetPagePtr + reqLen <= private->endptr)
			count = private->endptr - targetPagePtr;
		else
		{
			private->endptr_reached = true;
			return -1;
		}
	}
	XLogDumpXLogRead(private->inpath, private->timeline, targetPagePtr,
					readBuff, count);

	return count;
}

static void
split_path(const char *path, char **dir, char **fname)
{
	char       *sep;

	/* split filepath into directory & filename */
	sep = strrchr(path, '/');

	/* directory path */
	if (sep != NULL)
	{
		*dir = pg_strdup(path);
		(*dir)[(sep - path) + 1] = '\0';        /* no strndup */
		*fname = pg_strdup(sep + 1);
	}
	/* local directory */
	else
	{
		*dir = NULL;
		*fname = pg_strdup(path);
	}
}

static int
open_file_in_directory(const char *directory, const char *fname)
{
	int                     fd = -1;
	char            fpath[MAXPGPATH];

	Assert(directory != NULL);

	snprintf(fpath, MAXPGPATH, "%s/%s", directory, fname);
	fd = open(fpath, O_RDONLY | PG_BINARY, 0);

	if (fd < 0)
		fatal_error("could not open file \"%s\": %s",
				fname, strerror(errno));
	return fd;
}

static void
usage(void)
{
	printf(_("%s: Extract Full Page from write-ahead logs for debugging.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION] <file_path>\n"), progname);
	printf(_("  -s, --start=RECPTR start extracting at WAL location RECPTR\n"));
	printf(_("  -e, --end=RECPTR   stop extracting at WAL location RECPTR\n"));
	printf(_("  -x, --xid=XID      only extract from records with transaction ID XID\n"));
	printf(_("  -r, --rel=RID      only extract pages with relation ID RID\n"));
	printf(_("  -d, --dest         destination directory for extracted block(s) (default /tmp) \n"));
	printf(_("  -c, --check        generate checksum in page dump (default false)\n"));
	printf(_("  -t, --test         dryrun (display only)\n"));
}


int
main(int argc, char **argv)
{
	uint32 xlogid;
	uint32 xrecoff;
	XLogReaderState *xlogreader_state;
	XLogDumpPrivate private;
	XLogDumpConfig config;
	XLogRecord *record;
	XLogRecPtr      first_record;
	char       *errormsg;

	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"check", no_argument, NULL, 'c'},
		{"test", no_argument, NULL, 't'},
		{"end", required_argument, NULL, 'e'},
		{"start", required_argument, NULL, 's'},
		{"xid", required_argument, NULL, 'x'},
		{"relid", required_argument, NULL, 'r'},
		{"dest", required_argument, NULL, 'd'},
		{NULL, 0, NULL, 0}
	};

	int                     option;
	int                     optindex = 0;
#if PG_VERSION_NUM >= 120000
	pg_logging_init(argv[0]);
#endif
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
	}

	memset(&private, 0, sizeof(XLogDumpPrivate));
	memset(&config, 0, sizeof(XLogDumpConfig));

	private.timeline = 1;
	private.startptr = InvalidXLogRecPtr;
	private.endptr = InvalidXLogRecPtr;
	private.endptr_reached = false;

	config.checksum = false;
	config.dryrun = false;
	config.dump_dest = "/tmp";
	config.filter_by_xid = InvalidTransactionId;
	config.filter_by_xid_enabled = false;
	config.filter_by_relid = 0;
	config.filter_by_relid_enabled = false;

	if (argc <= 1)
	{
#if PG_VERSION_NUM >= 120000
		pg_log_error("no arguments specified");
#else
		fprintf(stderr, _("%s: no arguments specified\n"), progname);
#endif
		goto bad_argument;
	}

	while ((option = getopt_long(argc, argv, "tcx:r:d:e:s:",
							long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 't':
				config.dryrun = true;
				break;
			case 'c':
				config.checksum = true;
				break;
			case 'x':
				if (sscanf(optarg, "%u", &config.filter_by_xid) != 1)
				{
#if PG_VERSION_NUM >= 120000
					pg_log_error("could not parse \"%s\" as a transaction ID", optarg);
#else
					fprintf(stderr, _("%s: could not parse \"%s\" as a transaction ID\n"), progname, optarg);
#endif
					goto bad_argument;
				}
				config.filter_by_xid_enabled = true;
				break;
			case 'r':
				if (sscanf(optarg, "%u", &config.filter_by_relid) != 1)
				{
#if PG_VERSION_NUM >= 120000
					pg_log_error("could not parse \"%s\" as a relation ID", optarg);
#else
					fprintf(stderr, _("%s: could not parse \"%s\" as a relation ID\n"), progname, optarg);
#endif
					goto bad_argument;
				}
				config.filter_by_relid_enabled = true;
				break;
			case 'd':
				config.dump_dest = pg_strdup(optarg);
				break;
			case 'e':
				if (sscanf(optarg, "%X/%X", &xlogid, &xrecoff) != 2)
				{
#if PG_VERSION_NUM >= 120000
					pg_log_error("could not parse end WAL location \"%s\"",
						optarg);
#else
					fprintf(stderr, _("%s: could not parse end WAL location \"%s\"\n"), progname, optarg);
#endif
					goto bad_argument;
				}
				private.endptr = (uint64) xlogid << 32 | xrecoff;
				break;
			case 's':
				if (sscanf(optarg, "%X/%X", &xlogid, &xrecoff) != 2)
				{
#if PG_VERSION_NUM >= 120000
					pg_log_error("could not parse start WAL location \"%s\"",
						optarg);
#else
					fprintf(stderr, _("%s: could not parse start WAL location \"%s\"\n"), progname, optarg);
#endif
					goto bad_argument;
				}
				private.startptr = (uint64) xlogid << 32 | xrecoff;
				break;
			default:
				goto bad_argument;
		}
	}

	if (optind < argc)
	{
		char       *directory = NULL;
		char       *fname = NULL;
		int                     fd;
		XLogSegNo       segno;
#if PG_VERSION_NUM >= 110000
		PGAlignedXLogBlock buf;
		int                     r;
#endif
		split_path(argv[optind], &directory, &fname);

		if (directory != NULL)
			private.inpath = directory;
		else
			private.inpath = pg_strdup(".");

		fd = open_file_in_directory(private.inpath, fname);
		if (fd < 0) 
			fatal_error("could not open file \"%s\"", fname);

#if PG_VERSION_NUM >= 110000
		r = read(fd, buf.data, XLOG_BLCKSZ);
		if (r == XLOG_BLCKSZ)
		{
			XLogLongPageHeader longhdr = (XLogLongPageHeader) buf.data;

			WalSegSz = longhdr->xlp_seg_size;

			if (!IsValidWalSegSize(WalSegSz))
				fatal_error(ngettext("WAL segment size must be a power of two between 1 MB and 1 GB, but the WAL file \"%s\" header specifies %d byte",
				"WAL segment size must be a power of two between 1 MB and 1 GB, but the WAL file \"%s\" header specifies %d bytes",
				WalSegSz),
				fname, WalSegSz);
		}
		else
		{
			if (errno != 0)
				fatal_error("could not read file \"%s\": %s",
				fname, strerror(errno));
			else
				fatal_error("could not read file \"%s\": read %d of %zu",
				fname, r, (Size) XLOG_BLCKSZ);
		}
		close(fd);
		XLogFromFileName(fname, &private.timeline, &segno, WalSegSz);
#else
		XLogFromFileName(fname, &private.timeline, &segno);
#endif
		if (XLogRecPtrIsInvalid(private.startptr))
#if PG_VERSION_NUM >= 110000
			XLogSegNoOffsetToRecPtr(segno, 0, WalSegSz, private.startptr);
			xlogreader_state = XLogReaderAllocate(WalSegSz, XLogDumpReadPage, &private);
#else
			XLogSegNoOffsetToRecPtr(segno, 0, private.startptr);
			xlogreader_state = XLogReaderAllocate(XLogDumpReadPage, &private);
#endif
			if (!xlogreader_state)
				fatal_error("out of memory");

			/* first find a valid recptr to start from */
			first_record = XLogFindNextRecord(xlogreader_state, private.startptr);

			if (first_record == InvalidXLogRecPtr)
				fatal_error("could not find a valid record after %X/%X",
					(uint32) (private.startptr >> 32),
					(uint32) private.startptr);

			if (first_record != private.startptr &&
#if PG_VERSION_NUM >= 110000
					XLogSegmentOffset(private.startptr, WalSegSz) != 0)
#else
					(private.startptr % XLogSegSize) != 0)
#endif
				printf(ngettext("first record is after %X/%X, at %X/%X, skipping over %u byte\n",
							"first record is after %X/%X, at %X/%X, skipping over %u bytes\n",
							(first_record - private.startptr)),
							(uint32) (private.startptr >> 32), (uint32) private.startptr,
							(uint32) (first_record >> 32), (uint32) first_record,
							(uint32) (first_record - private.startptr));
			for (;;)
			{
				/* try to read the next record */
				record = XLogReadRecord(xlogreader_state, first_record, &errormsg);
				if (!record)
					break;
				/* after reading the first record, continue at next one */
				first_record = InvalidXLogRecPtr;

				/* apply all specified filters */
				if (config.filter_by_xid_enabled &&
					config.filter_by_xid != record->xl_xid)
					continue;

				pgwfe_XLogDumpRecord(&config, xlogreader_state);
			}
	}
	return EXIT_SUCCESS;
bad_argument:
	fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
	return EXIT_FAILURE;
}
