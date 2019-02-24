/*
 * arrow_read.c - routines to parse apache arrow file
 *
 * Copyright 2018-2019 (C) KaiGai Kohei <kaigai@heterodb.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the PostgreSQL License. See the LICENSE file.
 */
#include "pg_strom.h"
#include "arrow_defs.h"

/* table/vtable of FlatBuffer */
typedef struct
{
	uint16		vlen;	/* vtable length */
	uint16		tlen;	/* table length */
	uint16		offset[FLEXIBLE_ARRAY_MEMBER];
} FBVtable;

typedef struct
{
	int32	   *table;
	FBVtable   *vtable;
} FBTable;

typedef struct
{
	int32		metaLength;
	int32		headOffset;
} FBMetaData;

static inline FBTable
fetchFBTable(void *p_table)
{
	FBTable		t;

	t.table  = (int32 *)p_table;
	t.vtable = (FBVtable *)((char *)p_table - *t.table);

	return t;
}

static inline void *
__fetchPointer(FBTable *t, int index)
{
	FBVtable   *vtable = t->vtable;

	if (offsetof(FBVtable, offset[index]) < vtable->vlen)
	{
		int		offset = vtable->offset[index];
		if (offset)
		{
			void   *addr = (char *)t->table + offset;

			assert((char *)addr < (char *)t->table + vtable->tlen);
			return addr;
		}
	}
	return NULL;
}

static inline bool
fetchBool(FBTable *t, int index)
{
	bool	   *ptr = __fetchPointer(t, index);
	return (ptr ? *ptr : false);
}

static inline int8
fetchChar(FBTable *t, int index)
{
	int8	   *ptr = __fetchPointer(t, index);
	return (ptr ? *ptr : 0);
}

static inline int16
fetchShort(FBTable *t, int index)
{
	int16	  *ptr = __fetchPointer(t, index);
	return (ptr ? *ptr : 0);
}

static inline int32
fetchInt(FBTable *t, int index)
{
	int32	  *ptr = __fetchPointer(t, index);
	return (ptr ? *ptr : 0);
}

static inline int64
fetchLong(FBTable *t, int index)
{
	int64	  *ptr = __fetchPointer(t, index);
	return (ptr ? *ptr : 0);
}

static inline void *
fetchOffset(FBTable *t, int index)
{
	int32  *ptr = __fetchPointer(t, index);
	return (ptr ? (char *)ptr + *ptr : NULL);
}

static inline const char *
fetchString(FBTable *t, int index, int *p_strlen)
{
	int32  *ptr = fetchOffset(t, index);
	int32	len = 0;
	char   *temp;

	if (!ptr)
		temp = NULL;
	else
	{
		len = *ptr++;
		temp = pnstrdup((char *)ptr, len);
	}
	*p_strlen = len;
	return temp;
}

static inline int32 *
fetchVector(FBTable *t, int index, int *p_nitems)
{
	int32  *vector = fetchOffset(t, index);

	if (!vector)
		*p_nitems = 0;
	else
		*p_nitems = *vector++;
	return vector;
}

static void
readArrowKeyValue(ArrowKeyValue *kv, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *)pos);

	memset(kv, 0, sizeof(ArrowKeyValue));
	kv->tag		= ArrowNodeTag__KeyValue;
	kv->key     = fetchString(&t, 0, &kv->_key_len);
	kv->value   = fetchString(&t, 1, &kv->_value_len);
}

static void
readArrowTypeInt(ArrowTypeInt *node, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *) pos);

	node->bitWidth  = fetchInt(&t, 0);
	node->is_signed = fetchBool(&t, 1);
}

static void
readArrowTypeFloatingPoint(ArrowTypeFloatingPoint *node, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *) pos);

	node->precision = fetchShort(&t, 0);
}

static void
readArrowTypeDecimal(ArrowTypeDecimal *node, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *) pos);

	node->precision = fetchInt(&t, 0);
	node->scale     = fetchInt(&t, 1);
}

static void
readArrowTypeDate(ArrowTypeDate *node, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *) pos);
	int16	   *ptr;

	/* Date->unit has non-zero default value */
	ptr = __fetchPointer(&t, 0);
	node->unit = (ptr != NULL ? *ptr : ArrowDateUnit__MilliSecond);
}

static void
readArrowTypeTime(ArrowTypeTime *node, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *) pos);

	node->unit = fetchShort(&t, 0);
	node->bitWidth = fetchInt(&t, 1);
}

static void
readArrowTypeTimestamp(ArrowTypeTimestamp *node, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *) pos);

	node->unit = fetchInt(&t, 0);
	node->timezone = fetchString(&t, 1, &node->_timezone_len);
}

static void
readArrowTypeInterval(ArrowTypeInterval *node, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *) pos);

	node->unit = fetchShort(&t, 0);
}

static void
readArrowTypeUnion(ArrowTypeUnion *node, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *) pos);
	int32	   *vector;
	int32		nitems;

	node->mode = fetchShort(&t, 0);
	vector = fetchVector(&t, 1, &nitems);
	if (nitems == 0)
		node->typeIds = NULL;
	else
	{
		node->typeIds = palloc0(sizeof(int32) * nitems);
		memcpy(node->typeIds, vector, sizeof(int32) * nitems);
	}
	node->_num_typeIds = nitems;
}

static void
readArrowTypeFixedSizeBinary(ArrowTypeFixedSizeBinary *node, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *) pos);

	node->byteWidth = fetchInt(&t, 0);
}

static void
readArrowTypeFixedSizeList(ArrowTypeFixedSizeList *node, const char *pos)
{
	FBTable		t= fetchFBTable((int32 *) pos);

	node->listSize = fetchInt(&t, 0);
}

static void
readArrowTypeMap(ArrowTypeMap *node, const char *pos)
{
	FBTable		t= fetchFBTable((int32 *) pos);

	node->keysSorted = fetchBool(&t, 0);
}

static void
readArrowType(ArrowType *type, int type_tag, const char *type_pos)
{
	memset(type, 0, sizeof(ArrowType));
	switch (type_tag)
	{
		case ArrowType__Null:
			type->tag = ArrowNodeTag__Null;
			break;
		case ArrowType__Int:
			type->tag = ArrowNodeTag__Int;
			if (type_pos)
				readArrowTypeInt(&type->Int, type_pos);
			break;
		case ArrowType__FloatingPoint:
			type->tag = ArrowNodeTag__FloatingPoint;
			if (type_pos)
				readArrowTypeFloatingPoint(&type->FloatingPoint, type_pos);
			break;
		case ArrowType__Binary:
			type->tag = ArrowNodeTag__Binary;
			break;
		case ArrowType__Utf8:
			type->tag = ArrowNodeTag__Utf8;
			break;
		case ArrowType__Bool:
			type->tag = ArrowNodeTag__Bool;
			break;
		case ArrowType__Decimal:
			type->tag = ArrowNodeTag__Decimal;
			if (type_pos)
				readArrowTypeDecimal(&type->Decimal, type_pos);
			break;
		case ArrowType__Date:
			type->tag = ArrowNodeTag__Date;
			if (type_pos)
				readArrowTypeDate(&type->Date, type_pos);
			break;
		case ArrowType__Time:
			type->tag = ArrowNodeTag__Time;
			if (type_pos)
				readArrowTypeTime(&type->Time, type_pos);
			break;
		case ArrowType__Timestamp:
			type->tag = ArrowNodeTag__Timestamp;
			if (type_pos)
				readArrowTypeTimestamp(&type->Timestamp, type_pos);
			break;
		case ArrowType__Interval:
			type->tag = ArrowNodeTag__Interval;
			if (type_pos)
				readArrowTypeInterval(&type->Interval, type_pos);
			break;
		case ArrowType__List:
			type->tag = ArrowNodeTag__List;
			break;
		case ArrowType__Struct:
			type->tag = ArrowNodeTag__Struct;
			break;
		case ArrowType__Union:
			type->tag = ArrowNodeTag__Union;
			if (type_pos)
				readArrowTypeUnion(&type->Union, type_pos);
			break;
		case ArrowType__FixedSizeBinary:
			type->tag = ArrowNodeTag__FixedSizeBinary;
			if (type_pos)
				readArrowTypeFixedSizeBinary(&type->FixedSizeBinary, type_pos);
			break;
		case ArrowType__FixedSizeList:
			type->tag = ArrowNodeTag__FixedSizeList;
			if (type_pos)
				readArrowTypeFixedSizeList(&type->FixedSizeList, type_pos);
			break;
		case ArrowType__Map:
			type->tag = ArrowNodeTag__Map;
			if (type_pos)
				readArrowTypeMap(&type->Map, type_pos);
			break;
		default:
			printf("no suitable ArrowType__* tag for the code = %d", type_tag);
			break;
	}
}

static void
readArrowDictionaryEncoding(ArrowDictionaryEncoding *dict, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *)pos);
	const char *type_pos;

	memset(dict, 0, sizeof(ArrowDictionaryEncoding));
	dict->tag		= ArrowNodeTag__DictionaryEncoding;
	dict->id		= fetchLong(&t, 0);
	type_pos		= fetchOffset(&t, 1);
	dict->indexType.tag = ArrowNodeTag__Int;
	readArrowTypeInt(&dict->indexType, type_pos);
	dict->isOrdered	= fetchBool(&t, 2);
}

static void
readArrowField(ArrowField *field, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *)pos);
	int			type_tag;
	const char *type_pos;
	const char *dict_pos;
	int32	   *vector;
	int			i, nitems;

	memset(field, 0, sizeof(ArrowField));
	field->tag		= ArrowNodeTag__Field;
	field->name		= fetchString(&t, 0, &field->_name_len);
	field->nullable	= fetchBool(&t, 1);
	/* type */
	type_tag		= fetchChar(&t, 2);
	type_pos		= fetchOffset(&t, 3);
	readArrowType(&field->type, type_tag, type_pos);

	/* dictionary */
	dict_pos = fetchOffset(&t, 4);
	if (dict_pos)
		readArrowDictionaryEncoding(&field->dictionary, dict_pos);

	/* children */
	vector = fetchVector(&t, 5, &nitems);
	if (nitems > 0)
	{
		field->children = palloc0(sizeof(ArrowField) * nitems);
		for (i=0; i < nitems; i++)
		{
			int		offset = vector[i];

			if (offset == 0)
				elog(ERROR, "ArrowField has NULL-element in children[]");
			readArrowField(&field->children[i],
						   (const char *)&vector[i] + offset);
		}
	}
	field->_num_children = nitems;

	/* custom_metadata */
	vector = fetchVector(&t, 6, &nitems);
	if (nitems > 0)
	{
		field->custom_metadata = palloc0(sizeof(ArrowKeyValue) * nitems);
		for (i=0; i < nitems; i++)
		{
			int		offset = vector[i];

			if (offset == 0)
				elog(ERROR, "ArrowField has NULL-element in custom_metadata[]");
			readArrowKeyValue(&field->custom_metadata[i],
							  (const char *)&vector[i] + offset);
		}
	}
	field->_num_custom_metadata = nitems;
}

static void
readArrowSchema(ArrowSchema *schema, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *)pos);
	int32	   *vector;
	int32		i, nitems;

	memset(schema, 0, sizeof(ArrowSchema));
	schema->tag			= ArrowNodeTag__Schema;
	schema->endianness	= fetchBool(&t, 0);
	/* [ fields ]*/
	vector = fetchVector(&t, 1, &nitems);
	if (nitems > 0)
	{
		schema->fields = palloc0(sizeof(ArrowField) * nitems);
		for (i=0; i < nitems; i++)
		{
			int		offset = vector[i];

			if (offset == 0)
				elog(ERROR, "ArrowSchema has NULL-element in fields[]");
			readArrowField(&schema->fields[i],
						   (const char *)&vector[i] + offset);
		}
	}
	schema->_num_fields = nitems;

	/* [ custom_metadata ] */
	vector = fetchVector(&t, 2, &nitems);
	if (nitems > 0)
	{
		schema->custom_metadata = palloc0(sizeof(ArrowKeyValue) * nitems);
		for (i=0; i < nitems; i++)
		{
			int		offset = vector[i];

			if (offset == 0)
				elog(ERROR, "ArrowSchema has NULL-element in custom_metadata[]");
			readArrowKeyValue(&schema->custom_metadata[i],
							  (const char *)&vector[i] + offset);
		}
	}
	schema->_num_custom_metadata = nitems;
}

static size_t
readArrowFieldNode(ArrowFieldNode *node, const char *pos)
{
	struct {
		int64		length		__attribute__ ((aligned(8)));
		int64		null_count	__attribute__ ((aligned(8)));
	} *fmap = (void *) pos;

	memset(node, 0, sizeof(ArrowFieldNode));
	node->tag			= ArrowNodeTag__FieldNode;
	node->length		= fmap->length;
	node->null_count	= fmap->null_count;

	return sizeof(*fmap);
}

static size_t
readArrowBuffer(ArrowBuffer *node, const char *pos)
{
	struct {
		int64		offset		__attribute__ ((aligned(8)));
		int64		length		__attribute__ ((aligned(8)));
	} *fmap = (void *) pos;

	memset(node, 0, sizeof(ArrowBuffer));
	node->tag			= ArrowNodeTag__Buffer;
	node->offset		= fmap->offset;
	node->length		= fmap->length;

	return sizeof(*fmap);


}

static void
readArrowRecordBatch(ArrowRecordBatch *rbatch, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *)pos);
	const char *next;
	int			i, nitems;

	memset(rbatch, 0, sizeof(ArrowRecordBatch));
	rbatch->tag		= ArrowNodeTag__RecordBatch;
	rbatch->length	= fetchLong(&t, 0);
	/* nodes: [FieldNode] */
	next = (const char *)fetchVector(&t, 1, &nitems);
	if (nitems > 0)
	{
		rbatch->nodes = palloc0(sizeof(ArrowFieldNode) * nitems);
		for (i=0; i < nitems; i++)
			next += readArrowFieldNode(&rbatch->nodes[i], next);
	}
	rbatch->_num_nodes = nitems;

	/* buffers: [Buffer] */
	next = (const char *)fetchVector(&t, 2, &nitems);
	if (nitems > 0)
	{
		rbatch->buffers = palloc0(sizeof(ArrowBuffer) * nitems);
		for (i=0; i < nitems; i++)
			next += readArrowBuffer(&rbatch->buffers[i], next);
	}
	rbatch->_num_buffers = nitems;
}

static void
readArrowDictionaryBatch(ArrowDictionaryBatch *dbatch, const char *pos)
{
	FBTable		t = fetchFBTable((int32 *)pos);
	const char *next;

	memset(dbatch, 0, sizeof(ArrowDictionaryBatch));
	dbatch->tag	= ArrowNodeTag__DictionaryBatch;
	dbatch->id	= fetchLong(&t, 0);
	next		= fetchOffset(&t, 1);
	readArrowRecordBatch(&dbatch->data, next);
	dbatch->isDelta = fetchBool(&t, 2);
}

static void
readArrowMessage(ArrowMessage *message, const char *pos)
{
	FBTable			t = fetchFBTable((int32 *)pos);
	int				mtype;
	const char	   *next;

	memset(message, 0, sizeof(ArrowMessage));
	message->tag		= ArrowNodeTag__Message;
	message->version	= fetchShort(&t, 0);
	mtype				= fetchChar(&t, 1);
	next				= fetchOffset(&t, 2);
	message->bodyLength	= fetchLong(&t, 3);

	if (message->version != ArrowMetadataVersion__V4)
		elog(ERROR, "metadata version %d is not supported", message->version);

	switch (mtype)
	{
		case ArrowMessageHeader__Schema:
			readArrowSchema(&message->body.schema, next);
			break;
		case ArrowMessageHeader__DictionaryBatch:
			readArrowDictionaryBatch(&message->body.dictionaryBatch, next);
			break;
		case ArrowMessageHeader__RecordBatch:
			readArrowRecordBatch(&message->body.recordBatch, next);
			break;
		case ArrowMessageHeader__Tensor:
			elog(ERROR, "message type: Tensor is not implemented");
			break;
		case ArrowMessageHeader__SparseTensor:
			elog(ERROR, "message type: SparseTensor is not implemented");
			break;
		default:
			elog(ERROR, "unknown message header type: %d", mtype);
			break;
	}
}

/*
 * readArrowBlock (read inline structure)
 */
static size_t
readArrowBlock(ArrowBlock *node, const char *pos)
{
	struct {
		int64		offset			__attribute__ ((aligned(8)));
		int32		metaDataLength	__attribute__ ((aligned(8)));
		int64		bodyLength		__attribute__ ((aligned(8)));
	} *fmap = (void *) pos;

	memset(node, 0, sizeof(ArrowBlock));
	node->tag            = ArrowNodeTag__Block;
	node->offset         = fmap->offset;
	node->metaDataLength = fmap->metaDataLength;
	node->bodyLength     = fmap->bodyLength;

	return sizeof(*fmap);
}

/*
 * readArrowFooter
 */
static void
readArrowFooter(ArrowFooter *node, const char *pos)
{
	FBTable			t = fetchFBTable((int32 *)pos);
	const char	   *next;
	int				i, nitems;

	memset(node, 0, sizeof(ArrowFooter));
	node->tag		= ArrowNodeTag__Footer;
	node->version	= fetchShort(&t, 0);
	/* schema */
	next = fetchOffset(&t, 1);
	readArrowSchema(&node->schema, next);
	/* [dictionaries] */
	next = (const char *)fetchVector(&t, 2, &nitems);
	if (nitems > 0)
	{
		node->dictionaries = palloc0(sizeof(ArrowBlock) * nitems);
		for (i=0; i < nitems; i++)
			next += readArrowBlock(&node->dictionaries[i], next);
		node->_num_dictionaries = nitems;
	}

	/* [recordBatches] */
	next = (const char *)fetchVector(&t, 3, &nitems);
	if (nitems > 0)
	{
		node->recordBatches = palloc0(sizeof(ArrowBlock) * nitems);
		for (i=0; i < nitems; i++)
			next += readArrowBlock(&node->recordBatches[i], next);
		node->_num_recordBatches = nitems;
	}
}

/*
 * readArrowFile - read the supplied apache arrow file
 */
void
readArrowFile(const char *pathname, ArrowFileInfo *af_info)
{
	int				fdesc = -1;
	struct stat		st_buf;
	size_t			mmap_sz = 0;
	char		   *mmap_head = NULL;
	char		   *mmap_tail = NULL;
	const char	   *pos;
	FBMetaData	   *meta;
	cl_int			offset;
	cl_int			i, nitems;

	PG_TRY();
	{
		fdesc = open(pathname, O_RDONLY);
		if (fdesc < 0)
			elog(ERROR, "failed on open('%s'): %m", pathname);
		if (fstat(fdesc, &st_buf) != 0)
			elog(ERROR, "failed on fstat('%s'): %m", pathname);
		mmap_sz = TYPEALIGN(sysconf(_SC_PAGESIZE), st_buf.st_size);
		mmap_head = mmap(NULL, mmap_sz, PROT_READ, MAP_SHARED, fdesc, 0);
		if (mmap_head == MAP_FAILED)
			elog(ERROR, "failed on mmap('%s'): %m", pathname);
		mmap_tail = mmap_head + st_buf.st_size;

		/* check signature */
		if (memcmp(mmap_head, "ARROW1\0\0", 8) != 0 ||
			memcmp(mmap_tail - 6, "ARROW1", 6) != 0)
			elog(ERROR, "file signature mismatch on '%s'", pathname);

		/* OK, read the Apache Arrow file */
		memset(af_info, 0, sizeof(ArrowFileInfo));
		/* Read Footer chunk */
		pos = mmap_tail - 6 - sizeof(int32);
		offset = *((int32 *)pos);
		pos -= offset;
		offset = *((int32 *)pos);
		readArrowFooter(&af_info->footer, pos + offset);

		/* Read DictionaryBatch chunks */
		nitems = af_info->footer._num_dictionaries;
		for (i=0; i < nitems; i++)
		{
			ArrowBlock	   *b = &af_info->footer.dictionaries[i];
			ArrowMessage   *m = palloc0(sizeof(ArrowMessage));

			meta = (FBMetaData *)(mmap_head + b->offset);
			if (b->metaDataLength != meta->metaLength + sizeof(int32))
				elog(ERROR, "metadata length mismatch");
			pos = (const char *)&meta->headOffset + meta->headOffset;
			readArrowMessage(m, pos);

			af_info->dictionaries = lappend(af_info->dictionaries,
											&m->body.dictionaryBatch);
		}

		/* Read RecordBatch chunks */
		nitems = af_info->footer._num_recordBatches;
		for (i=0; i < nitems; i++)
		{
			ArrowBlock	   *b = &af_info->footer.recordBatches[i];
			ArrowMessage   *m = palloc0(sizeof(ArrowMessage));

			meta = (FBMetaData *)(mmap_head + b->offset);
			if (b->metaDataLength != meta->metaLength + sizeof(int32))
				elog(ERROR, "metadata length mismatch");
			pos = (const char *)&meta->headOffset + meta->headOffset;
			readArrowMessage(m, pos);

			af_info->recordBatches = lappend(af_info->recordBatches,
											 &m->body.recordBatch);
		}
	}
	PG_CATCH();
	{
		if (mmap_head != NULL && mmap_head != MAP_FAILED)
		{
			if (munmap(mmap_head, mmap_sz) != 0)
				elog(LOG, "failed on munmap(2): %m");
		}
		if (fdesc >= 0)
			close(fdesc);
	}
	PG_END_TRY();
	if (munmap(mmap_head, mmap_sz) != 0)
		elog(LOG, "failed on munmap(2): %m");
	close(fdesc);
}

/* ----------------------------------------------------------------
 *
 * Dump support for Apache Arrow nodes
 *
 * ----------------------------------------------------------------
 */
static void __dumpArrowNode(StringInfo str, ArrowNode *node)
{
	int		i;

	if (!node)
		return;
	switch (node->tag)
	{
	    case ArrowNodeTag__Null:
			appendStringInfo(str, "{Null}");
			break;

		case ArrowNodeTag__Int:
			{
				ArrowTypeInt   *t = (ArrowTypeInt *)node;

				appendStringInfo(str,"{%s%d}",
								 t->is_signed ? "Int" : "Uint",
								 t->bitWidth);
			}
			break;

		case ArrowNodeTag__FloatingPoint:
			{
				ArrowTypeFloatingPoint *f = (ArrowTypeFloatingPoint *)node;

				appendStringInfo(
					str,"{Float%s}",
					f->precision == ArrowPrecision__Half ? "16" :
					f->precision == ArrowPrecision__Single ? "32" :
					f->precision == ArrowPrecision__Double ? "64" : "??");
			}
			break;

		case ArrowNodeTag__Utf8:
			appendStringInfo(str,"{Utf8}");
			break;

		case ArrowNodeTag__Binary:
			appendStringInfo(str,"{Binary}");
			break;

		case ArrowNodeTag__Bool:
			appendStringInfo(str,"{Bool}");
			break;

		case ArrowNodeTag__Decimal:
			{
				ArrowTypeDecimal *d = (ArrowTypeDecimal *)node;

				appendStringInfo(str,"{Decimal: precision=%d, scale=%d}",
								 d->precision,
								 d->scale);
			}
			break;

		case ArrowNodeTag__Date:
			{
				ArrowTypeDate *d = (ArrowTypeDate *)node;

				appendStringInfo(
					str,"{Date: unit=%s}",
					d->unit == ArrowDateUnit__Day ? "day" :
					d->unit == ArrowDateUnit__MilliSecond ? "msec" : "???");
			}
			break;

		case ArrowNodeTag__Time:
			{
				ArrowTypeTime *t = (ArrowTypeTime *)node;

				appendStringInfo(
					str,"{Time: unit=%s}",
					t->unit == ArrowTimeUnit__Second ? "sec" :
					t->unit == ArrowTimeUnit__MilliSecond ? "ms" :
					t->unit == ArrowTimeUnit__MicroSecond ? "us" :
					t->unit == ArrowTimeUnit__NanoSecond ? "ns" : "???");
			}
			break;

		case ArrowNodeTag__Timestamp:
			{
				ArrowTypeTimestamp *t = (ArrowTypeTimestamp *)node;

				appendStringInfo(
					str,"{Timestamp: unit=%s}",
					t->unit == ArrowTimeUnit__Second ? "sec" :
					t->unit == ArrowTimeUnit__MilliSecond ? "ms" :
					t->unit == ArrowTimeUnit__MicroSecond ? "us" :
					t->unit == ArrowTimeUnit__NanoSecond ? "ns" : "???");
			}
			break;

		case ArrowNodeTag__Interval:
			{
				ArrowTypeInterval *t = (ArrowTypeInterval *)node;

				appendStringInfo(
					str,"{Interval: unit=%s}",
					t->unit==ArrowIntervalUnit__Year_Month ? "Year/Month" :
					t->unit==ArrowIntervalUnit__Day_Time ? "Day/Time" : "???");
			}
			break;

		case ArrowNodeTag__List:
			appendStringInfo(str,"{List}");
			break;

		case ArrowNodeTag__Struct:
			appendStringInfo(str,"{Struct}");
			break;

		case ArrowNodeTag__Union:
			{
				ArrowTypeUnion *u = (ArrowTypeUnion *)node;

				appendStringInfo(
					str,"{Union: mode=%s, typeIds=[",
					u->mode == ArrowUnionMode__Sparse ? "Sparse" :
					u->mode == ArrowUnionMode__Dense ? "Dense" : "???");
				for (i=0; i < u->_num_typeIds; i++)
					appendStringInfo(str, "%s%d", i > 0 ? ", " : " ",
									 u->typeIds[i]);
				appendStringInfo(str, "]}");
			}
			break;

		case ArrowNodeTag__FixedSizeBinary:
			appendStringInfo(
				str,"{FixedSizeBinary: byteWidth=%d}",
				((ArrowTypeFixedSizeBinary *)node)->byteWidth);
			break;

		case ArrowNodeTag__FixedSizeList:
			appendStringInfo(
				str,"{FixedSizeList: listSize=%d}",
				((ArrowTypeFixedSizeList *)node)->listSize);
			break;

		case ArrowNodeTag__Map:
			appendStringInfo(
				str,"{Map: keysSorted=%s}",
				((ArrowTypeMap *)node)->keysSorted ? "true" : "false");
			break;

		case ArrowNodeTag__KeyValue:
			{
				ArrowKeyValue *kv = (ArrowKeyValue *)node;

				appendStringInfo(str,"{KeyValue: key=\"");
				if (kv->key)
					outToken(str, kv->key);
				appendStringInfo(str,"\", value=\"");
				if (kv->value)
					outToken(str, kv->value);
				appendStringInfo(str,"\"}");
			}
			break;

		case ArrowNodeTag__DictionaryEncoding:
			{
				ArrowDictionaryEncoding *d = (ArrowDictionaryEncoding *)node;

				appendStringInfo(str,"{DictionaryEncoding: id=%ld, indexType=",
								 d->id);
				__dumpArrowNode(str, (ArrowNode *)&d->indexType);
				appendStringInfo(str,", isOrdered=%s}",
								 d->isOrdered ? "true" : "false");
			}
			break;

		case ArrowNodeTag__Field:
			{
				ArrowField *f = (ArrowField *)node;

				appendStringInfo(str, "{Field: name=\"");
				if (f->name)
					outToken(str, f->name);
				appendStringInfo(str, "\", nullable=%s, type=",
								 f->nullable ? "true" : "false");
				__dumpArrowNode(str, (ArrowNode *)&f->type);
				if (f->dictionary.indexType.tag == ArrowNodeTag__Int)
				{
					appendStringInfo(str, ", dictionary=");
					__dumpArrowNode(str, (ArrowNode *)&f->dictionary);
				}
				appendStringInfo(str, ", children=[");
				for (i=0; i < f->_num_children; i++)
				{
					if (i > 0)
						appendStringInfo(str, ", ");
					__dumpArrowNode(str, (ArrowNode *)&f->children[i]);
				}
				appendStringInfo(str, "], custom_metadata=[");
				for (i=0; i < f->_num_custom_metadata; i++)
				{
					if (i > 0)
						appendStringInfo(str, ", ");
					__dumpArrowNode(str, (ArrowNode *)&f->custom_metadata[i]);
				}
				appendStringInfo(str, "]}");
			}
			break;

		case ArrowNodeTag__FieldNode:
			appendStringInfo(
				str, "{FieldNode: length=%ld, null_count=%ld}",
				((ArrowFieldNode *)node)->length,
				((ArrowFieldNode *)node)->null_count);
			break;

		case ArrowNodeTag__Buffer:
			appendStringInfo(
				str, "{Buffer: offset=%ld, length=%ld}",
				((ArrowBuffer *)node)->offset,
				((ArrowBuffer *)node)->length);
			break;

		case ArrowNodeTag__Schema:
			{
				ArrowSchema *s = (ArrowSchema *)node;

				appendStringInfo(
					str, "{Schema: endianness=%s, fields=[",
					s->endianness == ArrowEndianness__Little ? "little" :
					s->endianness == ArrowEndianness__Big ? "big" : "???");
				for (i=0; i < s->_num_fields; i++)
				{
					if (i > 0)
						appendStringInfo(str, ", ");
					__dumpArrowNode(str, (ArrowNode *)&s->fields[i]);
				}
				appendStringInfo(str, "], custom_metadata=[");
				for (i=0; i < s->_num_custom_metadata; i++)
				{
					if (i > 0)
						appendStringInfo(str, ", ");
					__dumpArrowNode(str, (ArrowNode *)&s->custom_metadata[i]);
				}
				appendStringInfo(str, "]}");
			}
			break;

		case ArrowNodeTag__RecordBatch:
			{
				ArrowRecordBatch *r = (ArrowRecordBatch *) node;

				appendStringInfo(str, "{RecordBatch: length=%ld, nodes=[",
								 r->length);
				for (i=0; i < r->_num_nodes; i++)
				{
					if (i > 0)
						appendStringInfo(str, ", ");
					__dumpArrowNode(str, (ArrowNode *)&r->nodes[i]);
				}
				appendStringInfo(str, "], buffers=[");
				for (i=0; i < r->_num_buffers; i++)
				{
					if (i > 0)
						appendStringInfo(str, ", ");
					__dumpArrowNode(str, (ArrowNode *)&r->buffers[i]);
				}
				appendStringInfo(str,"]}");
			}
			break;

		case ArrowNodeTag__DictionaryBatch:
			{
				ArrowDictionaryBatch *d = (ArrowDictionaryBatch *)node;

				appendStringInfo(str, "{DictionaryBatch: id=%ld, data=",
								 d->id);
				__dumpArrowNode(str, (ArrowNode *)&d->data);
				appendStringInfo(str, ", isDelta=%s}",
								 d->isDelta ? "true" : "false");
			}
			break;

		case ArrowNodeTag__Message:
			{
				ArrowMessage *m = (ArrowMessage *)node;

				appendStringInfo(
					str, "{Message: version=%s, body=",
					m->version == ArrowMetadataVersion__V1 ? "V1" :
					m->version == ArrowMetadataVersion__V2 ? "V2" :
					m->version == ArrowMetadataVersion__V3 ? "V3" :
					m->version == ArrowMetadataVersion__V4 ? "V4" : "???");
				__dumpArrowNode(str, (ArrowNode *)&m->body);
				appendStringInfo(str, ", bodyLength=%lu}", m->bodyLength);
			}
			break;

		case ArrowNodeTag__Block:
			appendStringInfo(
				str, "{Block: offset=%ld, metaDataLength=%d bodyLength=%ld}",
				((ArrowBlock *)node)->offset,
				((ArrowBlock *)node)->metaDataLength,
				((ArrowBlock *)node)->bodyLength);
			break;

		case ArrowNodeTag__Footer:
			{
				ArrowFooter *f = (ArrowFooter *)node;

				appendStringInfo(
					str, "{Footer: version=%s, schema=",
					f->version == ArrowMetadataVersion__V1 ? "V1" :
					f->version == ArrowMetadataVersion__V2 ? "V2" :
					f->version == ArrowMetadataVersion__V3 ? "V3" :
					f->version == ArrowMetadataVersion__V4 ? "V4" : "???");
				__dumpArrowNode(str, (ArrowNode *)&f->schema);
				appendStringInfo(str, ", dictionaries=[");
				for (i=0; i < f->_num_dictionaries; i++)
				{
					if (i > 0)
						appendStringInfo(str, ", ");
					__dumpArrowNode(str, (ArrowNode *)&f->dictionaries[i]);
				}
				appendStringInfo(str, ", recordBatches=[");
				for (i=0; i < f->_num_recordBatches; i++)
				{
					if (i > 0)
						appendStringInfo(str, ", ");
					__dumpArrowNode(str, (ArrowNode *)&f->recordBatches[i]);
				}
				appendStringInfo(str, "]}");
			}
			break;

		default:
			elog(ERROR, "unknown ArrowNodeTag: %d\n", (int)node->tag);
	}
}

char *
dumpArrowNode(ArrowNode *node)
{
	StringInfoData str;

	initStringInfo(&str);
	__dumpArrowNode(&str, node);

	return str.data;
}