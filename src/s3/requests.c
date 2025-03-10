/*-------------------------------------------------------------------------
 *
 * requests.c
 *		Implementation for S3 requests.
 *
 * Copyright (c) 2023, OrioleDATA Inc.
 *
 * IDENTIFICATION
 *	  contrib/orioledb/src/s3/requests.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "orioledb.h"

#include "s3/requests.h"

#include "common/base64.h"
#include "lib/stringinfo.h"
#include "utils/wait_event.h"

#include "curl/curl.h"
#include "openssl/hmac.h"
#include "openssl/sha.h"

PG_FUNCTION_INFO_V1(s3_get);
PG_FUNCTION_INFO_V1(s3_put);

static void
hmac_sha256(char *input, char *output, char *secretkey, int secretkeylen)
{
	HMAC_CTX   *ctx;
	unsigned int len;

	ctx = HMAC_CTX_new();
	HMAC_Init_ex(ctx, secretkey, secretkeylen, EVP_sha256(), NULL);
	HMAC_Update(ctx, (unsigned char *) input, strlen(input));
	HMAC_Final(ctx, (unsigned char *) output, &len);
	HMAC_CTX_free(ctx);

	Assert(len == 32);
}

/*
 * Make the hex representation of the binary string.
 */
static char *
hex_string(Pointer data, int len)
{
	char	   *result = palloc(len * 2 + 1);

	hex_encode(data, len, result);

	result[len * 2] = 0;
	return result;
}

/*
 * Calculate the hash of canocical request according to AWS4-HMAC-SHA256.
 */
static char *
canonical_request_hash(char *method, char *datetime, char *objectname,
					   char *contenthash)
{
	StringInfoData buf;
	unsigned char hash[32];

	initStringInfo(&buf);
	appendStringInfo(&buf, "%s\n", method);
	appendStringInfo(&buf, "/%s\n", objectname);
	appendStringInfo(&buf, "\n");
	appendStringInfo(&buf, "host:%s\n", s3_host);
	appendStringInfo(&buf, "x-amz-content-sha256:%s\n", contenthash);
	appendStringInfo(&buf, "x-amz-date:%s\n", datetime);
	appendStringInfo(&buf, "\n");
	appendStringInfo(&buf, "host;x-amz-content-sha256;x-amz-date\n");
	appendStringInfo(&buf, "%s", contenthash);

	(void) SHA256((unsigned char *) buf.data, buf.len, hash);
	pfree(buf.data);

	return hex_string((Pointer) hash, sizeof(hash));
}

/*
 * Construct signed string for the Authorization header,
 * following the Amazon S3 REST API spec.
 */
static char *
s3_signature(char *method, char *datetimestring, char *datestring,
			 char *objectname, char *secretkey, char *contenthash)
{
	StringInfoData buf;
	char	   *key;
	char		hash[32];
	char	   *chash;

	chash = canonical_request_hash(method, datetimestring,
								   objectname, contenthash);

	key = psprintf("AWS4%s", s3_secretkey);
	hmac_sha256(datestring, hash, key, strlen(key));
	hmac_sha256(s3_region, hash, hash, sizeof(hash));
	hmac_sha256("s3", hash, hash, sizeof(hash));
	hmac_sha256("aws4_request", hash, hash, sizeof(hash));

	initStringInfo(&buf);
	appendStringInfo(&buf, "AWS4-HMAC-SHA256\n");
	appendStringInfo(&buf, "%s\n", datetimestring);
	appendStringInfo(&buf, "%s/%s/s3/aws4_request\n", datestring, s3_region);
	appendStringInfo(&buf, "%s", chash);

	hmac_sha256(buf.data, hash, hash, sizeof(hash));

	pfree(key);
	pfree(chash);
	pfree(buf.data);

	return hex_string(hash, 32);
}

/*
 * Constructs GMT-style string for date.
 */
static char *
httpdate(time_t *timer)
{
	char	   *datetimestring;
	time_t		t;
	struct tm  *gt;

	t = time(timer);
	gt = gmtime(&t);
	datetimestring = (char *) palloc0(256 * sizeof(char));
	strftime(datetimestring, 256 * sizeof(char), "%Y%m%d", gt);
	return datetimestring;
}

/*
 * Constructs GMT-style string for date and time.
 */
static char *
httpdatetime(time_t *timer)
{
	char	   *datetimestring;
	time_t		t;
	struct tm  *gt;

	t = time(timer);
	gt = gmtime(&t);
	datetimestring = (char *) palloc0(256 * sizeof(char));
	strftime(datetimestring, 256 * sizeof(char), "%Y%m%dT%H%M%SZ", gt);
	return datetimestring;
}

/*
 * Curl callback, which appends data to String Info.
 */
static size_t
write_data_to_buf(void *buffer, size_t size, size_t nmemb, void *userp)
{
	size_t		segsize = size * nmemb;
	StringInfo	info = (StringInfo) userp;

	appendBinaryStringInfo(info, (const char *) buffer, segsize);

	return segsize;
}

/*
 * Get the binary content of an object from S3 into 'str'.
 */
static void
s3_get_object(char *objectname, StringInfo str)
{
	CURL	   *curl;
	char	   *url;
	char	   *datestring;
	char	   *datetimestring;
	char	   *signature;
	struct curl_slist *slist;
	char	   *tmp;
	int			sc;
	unsigned char hash[32];
	char	   *contenthash;
	long		http_code = 0;

	(void) SHA256(NULL, 0, hash);
	contenthash = hex_string((Pointer) hash, sizeof(hash));

	url = psprintf("https://%s/%s", s3_host, objectname);
	datestring = httpdate(NULL);
	datetimestring = httpdatetime(NULL);
	signature = s3_signature("GET", datetimestring, datestring, objectname,
							 s3_secretkey, contenthash);

	slist = NULL;
	slist = curl_slist_append(slist, (tmp = psprintf("x-amz-date: %s", datetimestring)));
	pfree(tmp);
	slist = curl_slist_append(slist, (tmp = psprintf("x-amz-content-sha256: %s", contenthash)));
	pfree(tmp);
	slist = curl_slist_append(slist,
							  (tmp = psprintf("Authorization: AWS4-HMAC-SHA256 Credential=%s/%s/%s/s3/aws4_request, SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=%s",
											  s3_accesskey, datestring, s3_region, signature)));
	pfree(tmp);

	curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	if (s3_cainfo)
		curl_easy_setopt(curl, CURLOPT_CAINFO, s3_cainfo);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data_to_buf);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, str);

	sc = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	if (sc != 0 || http_code != 200)
	{
		ereport(FATAL, (errcode(ERRCODE_CONNECTION_EXCEPTION),
						errmsg("could not get object from S3"),
						errdetail("return code = %d, http code = %ld, response = %s",
								  sc, http_code, str->data)));
	}

	curl_easy_cleanup(curl);

	curl_slist_free_all(slist);
	pfree(url);
	pfree(datestring);
	pfree(datetimestring);
	pfree(signature);
}

/*
 * A SQL function to get object from S3.  Currently only used for debugging
 * purposes.
 */
Datum
s3_get(PG_FUNCTION_ARGS)
{
	StringInfoData buf;

	initStringInfo(&buf);

	s3_get_object(text_to_cstring(PG_GETARG_TEXT_PP(0)), &buf);

	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

/*
 * Reads the part of the file 'filename' from 'offset' with length 'maxSize'.
 * The actual length might appear to be lower, it's to be written to '*size'.
 */
static Pointer
read_file_part(const char *filename, uint64 offset,
			   uint64 maxSize, uint64 *size)
{
	File		file;
	Pointer		buffer,
				ptr;
	uint64		totalSize;

	file = PathNameOpenFile(filename, O_RDONLY | PG_BINARY);
	if (file < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", filename)));
		return NULL;
	}

	totalSize = FileSize(file);
	totalSize = Min(totalSize, offset + maxSize);
	*size = Max(totalSize, offset) - offset;
	buffer = (Pointer) MemoryContextAllocHuge(CurrentMemoryContext, *size);

	ptr = buffer;
	while (offset < totalSize)
	{
		int			amount = Min(totalSize - offset, BLCKSZ);
		int			rc;

		rc = FileRead(file, ptr, amount, offset, WAIT_EVENT_DATA_FILE_READ);

		if (rc < 0)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", filename)));
			return NULL;
		}

		if (rc != amount)
		{
			amount = rc;
			*size = (ptr - buffer) + amount;
			break;
		}

		offset += amount;
		ptr += amount;
	}

	FileClose(file);

	return buffer;
}

/*
 * Writes the part of the file 'filename' from 'offset' with length 'size'.
 */
static void
write_file_part(const char *filename, uint64 offset,
				Pointer data, uint64 size)
{
	File		file;
	int			rc;

	file = PathNameOpenFile(filename, O_CREAT | O_RDWR | PG_BINARY);
	if (file < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", filename)));
		return;
	}

	rc = FileWrite(file, data, size, offset, WAIT_EVENT_DATA_FILE_WRITE);

	if (rc < 0 || rc != size)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", filename)));
		return;
	}

	FileWriteback(file, offset, size, WAIT_EVENT_DATA_FILE_FLUSH);

	FileClose(file);
}

/*
 * Read the whole file.
 */
static Pointer
read_file(const char *filename, uint64 *size)
{
	return read_file_part(filename, 0, UINT64_MAX, size);
}

/*
 * Put object with given binary contents to S3.
 */
static void
s3_put_object_with_contents(char *objectname, Pointer data, uint64 dataSize)
{
	CURL	   *curl;
	char	   *url;
	char	   *datestring;
	char	   *datetimestring;
	char	   *signature;
	char	   *contenthash;
	struct curl_slist *slist;
	char	   *tmp;
	int			sc;
	StringInfoData buf;
	unsigned char hash[32];
	long		http_code = 0;

	(void) SHA256((unsigned char *) data, dataSize, hash);
	contenthash = hex_string((Pointer) hash, sizeof(hash));

	url = psprintf("https://%s/%s", s3_host, objectname);
	datestring = httpdate(NULL);
	datetimestring = httpdatetime(NULL);
	signature = s3_signature("PUT", datetimestring, datestring, objectname,
							 s3_secretkey, contenthash);

	slist = NULL;
	slist = curl_slist_append(slist, (tmp = psprintf("x-amz-date: %s", datetimestring)));
	pfree(tmp);
	slist = curl_slist_append(slist, (tmp = psprintf("x-amz-content-sha256: %s", contenthash)));
	pfree(tmp);
	slist = curl_slist_append(slist, (tmp = psprintf("Content-Length: %lu", dataSize)));
	pfree(tmp);
	slist = curl_slist_append(slist,
							  (tmp = psprintf("Authorization: AWS4-HMAC-SHA256 Credential=%s/%s/%s/s3/aws4_request, SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=%s",
											  s3_accesskey, datestring, s3_region, signature)));
	slist = curl_slist_append(slist, "Content-Type: application/octet-stream");
	pfree(tmp);

	initStringInfo(&buf);

	curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	if (s3_cainfo)
		curl_easy_setopt(curl, CURLOPT_CAINFO, s3_cainfo);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, dataSize);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data_to_buf);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

	sc = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	if (sc != 0 || http_code != 200 || strlen(buf.data) != 0)
	{
		ereport(FATAL, (errcode(ERRCODE_CONNECTION_EXCEPTION),
						errmsg("could not put object to S3"),
						errdetail("return code = %d, http code = %ld, response = %s",
								  sc, http_code, buf.data)));
	}

	curl_easy_cleanup(curl);

	curl_slist_free_all(slist);
	if (data)
		pfree(data);
	pfree(url);
	pfree(datestring);
	pfree(datetimestring);
	pfree(signature);
	pfree(buf.data);
}

/*
 * Put the whole file as S3 object.
 */
bool
s3_put_file(char *objectname, char *filename)
{
	Pointer		data;
	uint64		dataSize = 0;

	data = read_file(filename, &dataSize);
	if (data)
		s3_put_object_with_contents(objectname, data, dataSize);
	return data != NULL;
}

/*
 * Put the file part as S3 object.
 */
bool
s3_put_file_part(char *objectname, char *filename, int partnum)
{
	Pointer		data;
	uint64		dataSize;

	data = read_file_part(filename,
						  partnum * ORIOLEDB_S3_PART_SIZE + ORIOLEDB_BLCKSZ,
						  ORIOLEDB_S3_PART_SIZE,
						  &dataSize);
	if (data)
		s3_put_object_with_contents(objectname, data, dataSize);
	return data != NULL;
}

/*
 * Get the file part from S3 object.
 */
void
s3_get_file_part(char *objectname, char *filename, int partnum)
{
	StringInfoData buf;

	initStringInfo(&buf);
	s3_get_object(objectname, &buf);

	write_file_part(filename,
					partnum * ORIOLEDB_S3_PART_SIZE + ORIOLEDB_BLCKSZ,
					buf.data,
					buf.len);

	pfree(buf.data);
}

/*
 * Put empty dir as S3 object.
 */
void
s3_put_empty_dir(char *objectname)
{
	s3_put_object_with_contents(objectname, NULL, 0);
}

/*
 * A SQL function to put object to S3.  Currently only used for debugging
 * purposes.
 */
Datum
s3_put(PG_FUNCTION_ARGS)
{
	char	   *objectname,
			   *filename;

	objectname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	filename = text_to_cstring(PG_GETARG_TEXT_PP(1));

	s3_put_file(objectname, filename);

	PG_RETURN_NULL();
}
