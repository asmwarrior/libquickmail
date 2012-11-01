#if defined(__WIN32__) && defined(DLL_EXPORT)
#define BUILD_QUICKMAIL_DLL
#endif
#include "quickmail.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifndef NOCURL
#include <curl/curl.h>
#else
#include "smtpsocket.h"
#endif

#define LIBQUICKMAIL_VERSION_MAJOR 0
#define LIBQUICKMAIL_VERSION_MINOR 1
#define LIBQUICKMAIL_VERSION_MICRO 7

#define VERSION_STRINGIZE_(major, minor, micro) #major"."#minor"."#micro
#define VERSION_STRINGIZE(major, minor, micro) VERSION_STRINGIZE_(major, minor, micro)

#define LIBQUICKMAIL_VERSION VERSION_STRINGIZE(LIBQUICKMAIL_VERSION_MAJOR,LIBQUICKMAIL_VERSION_MINOR,LIBQUICKMAIL_VERSION_MICRO)

#define NEWLINE "\r\n"
#define NEWLINELENGTH 2

#define MIME_LINE_WIDTH 72

//defines used for indicating the position status
#define MAILPART_INITIALIZE 0
#define MAILPART_HEADER     1
#define MAILPART_BODY       2
#define MAILPART_END        3
#define MAILPART_DONE       4

struct email_info_struct {
  int current;  //must be zet to 0
  time_t timestamp;
  char* from;
  struct email_info_string_list_struct* to;
  struct email_info_string_list_struct* cc;
  struct email_info_string_list_struct* bcc;
  char* subject;
  char* body;
  //struct email_info_string_list_struct* attachmentlist;
  struct email_info_attachment_list_struct* attachmentlist;
  char* buf;
  int buflen;
  char* mime_boundary;
  //struct email_info_string_list_struct* current_attachment;
  struct email_info_attachment_list_struct* current_attachment;
  FILE* debuglog;
  char dtable[64];
};

////////////////////////////////////////////////////////////////////////

struct email_info_string_list_struct {
  char* data;
  struct email_info_string_list_struct* next;
};

void email_info_string_list_add (struct email_info_string_list_struct** list, const char* data)
{
  struct email_info_string_list_struct** p = list;
  while (*p)
    p = &(*p)->next;
  *p = (struct email_info_string_list_struct*)malloc(sizeof(struct email_info_string_list_struct));
  (*p)->data = (data ? strdup(data) : NULL);
  (*p)->next = NULL;
}

void email_info_string_list_free (struct email_info_string_list_struct** list)
{
  struct email_info_string_list_struct* p = *list;
  struct email_info_string_list_struct* current;
  while (p) {
    current = p;
    p = current->next;
    free(current->data);
    free(current);
  }
  *list = NULL;
}

////////////////////////////////////////////////////////////////////////

typedef int (*email_info_attachment_open_fn)(struct email_info_attachment_list_struct* self);
typedef size_t (*email_info_attachment_read_fn)(struct email_info_attachment_list_struct* self, void* buf, size_t len);
typedef void (*email_info_attachment_close_fn)(struct email_info_attachment_list_struct* self);
typedef void (*email_info_attachment_extradata_free_fn)(struct email_info_attachment_list_struct* self);

struct email_info_attachment_list_struct {
  char* path;
  int isopen;
  void* extradata;
  email_info_attachment_open_fn email_info_attachment_open;
  email_info_attachment_read_fn email_info_attachment_read;
  email_info_attachment_close_fn email_info_attachment_close;
  email_info_attachment_extradata_free_fn email_info_attachment_extradata_free;
  struct email_info_attachment_list_struct* next;
};

int email_info_attachment_open_file (struct email_info_attachment_list_struct* self)
{
  return ((self->extradata = (void*)fopen(self->path, "rb")) != NULL ? 0 : -1);
}

size_t email_info_attachment_read_file (struct email_info_attachment_list_struct* self, void* buf, size_t len)
{
  return fread(buf, 1, len, (FILE*)self->extradata);
}

void email_info_attachment_close_file (struct email_info_attachment_list_struct* self)
{
  if ((FILE*)self->extradata) {
    fclose((FILE*)self->extradata);
    self->extradata = NULL;
  }
}

void email_info_attachment_list_add_file (struct email_info_attachment_list_struct** list, const char* path)
{
  struct email_info_attachment_list_struct** p = list;
  while (*p)
    p = &(*p)->next;
  *p = (struct email_info_attachment_list_struct*)malloc(sizeof(struct email_info_attachment_list_struct));
  (*p)->path = (path ? strdup(path) : NULL);
  (*p)->isopen = 0;
  (*p)->extradata = NULL;
  (*p)->email_info_attachment_open = email_info_attachment_open_file;
  (*p)->email_info_attachment_read = email_info_attachment_read_file;
  (*p)->email_info_attachment_close = email_info_attachment_close_file;
  (*p)->email_info_attachment_extradata_free = email_info_attachment_close_file;
  (*p)->next = NULL;
}

struct email_info_attachment_memory_extradata_struct {
  size_t pos;
  size_t datalen;
  const char* data;
};

int email_info_attachment_open_memory (struct email_info_attachment_list_struct* self)
{
  struct email_info_attachment_memory_extradata_struct* s = (struct email_info_attachment_memory_extradata_struct*)self->extradata;
  s->pos = 0;
  return (s->data ? 0 : -1);
}

size_t email_info_attachment_read_memory (struct email_info_attachment_list_struct* self, void* buf, size_t len)
{
  struct email_info_attachment_memory_extradata_struct* s = (struct email_info_attachment_memory_extradata_struct*)self->extradata;
  size_t n = (s->pos + len <= s->datalen ? len : s->datalen - s->pos);
  memcpy(buf, s->data + s->pos, n);
  s->pos += n;
  return n;
}

void email_info_attachment_list_add_memory (struct email_info_attachment_list_struct** list, const char* filename, const char* data, size_t datalen)
{
  struct email_info_attachment_list_struct** p = list;
  while (*p)
    p = &(*p)->next;
  *p = (struct email_info_attachment_list_struct*)malloc(sizeof(struct email_info_attachment_list_struct));
  (*p)->path = (filename ? strdup(filename) : NULL);
  (*p)->isopen = 0;
  (*p)->extradata = malloc(sizeof(struct email_info_attachment_memory_extradata_struct));
  ((struct email_info_attachment_memory_extradata_struct*)(*p)->extradata)->pos = 0;
  ((struct email_info_attachment_memory_extradata_struct*)(*p)->extradata)->datalen = datalen;
  ((struct email_info_attachment_memory_extradata_struct*)(*p)->extradata)->data = data;
  (*p)->email_info_attachment_open = email_info_attachment_open_memory;
  (*p)->email_info_attachment_read = email_info_attachment_read_memory;
  (*p)->email_info_attachment_close = NULL;
  (*p)->email_info_attachment_extradata_free = NULL;
  (*p)->next = NULL;
}

void email_info_attachment_list_free (struct email_info_attachment_list_struct** list)
{
  struct email_info_attachment_list_struct* p = *list;
  struct email_info_attachment_list_struct* current;
  while (p) {
    current = p;
    p = current->next;
    if (current->email_info_attachment_extradata_free)
      current->email_info_attachment_extradata_free(current);
    free(current->path);
    free(current->extradata);
    free(current);
  }
  *list = NULL;
}

////////////////////////////////////////////////////////////////////////

char* randomize_zeros (char* data)
{
  //replace all 0s with random digits
  char* p = data;
  while (*p) {
    if (*p == '0')
      *p = '0' + rand() % 10;
    p++;
  }
  return data;
}

char* str_append (char** data, const char* newdata)
{
  //append a string to the end of an existing string
  int len = (*data ? strlen(*data) : 0);
  *data = (char*)realloc(*data, len + strlen(newdata) + 1);
  strcpy(*data + len, newdata);
  return *data;
}

////////////////////////////////////////////////////////////////////////

DLL_EXPORT_LIBQUICKMAIL const char* quickmail_get_version ()
{
  return VERSION_STRINGIZE(LIBQUICKMAIL_VERSION_MAJOR, LIBQUICKMAIL_VERSION_MINOR, LIBQUICKMAIL_VERSION_MICRO);
}

DLL_EXPORT_LIBQUICKMAIL quickmail quickmail_create (const char* from, const char* subject)
{
  int i;
  struct email_info_struct* mailobj = (struct email_info_struct*)malloc(sizeof(struct email_info_struct));
  mailobj->current = 0;
  mailobj->timestamp = time(NULL);
  mailobj->from = (from ? strdup(from) : NULL);
  mailobj->to = NULL;
  mailobj->cc = NULL;
  mailobj->bcc = NULL;
  mailobj->subject = (subject ? strdup(subject) : NULL);
  mailobj->body = NULL;
  mailobj->attachmentlist = NULL;
  mailobj->buf = NULL;
  mailobj->buflen = 0;
  mailobj->mime_boundary = NULL;
  mailobj->current_attachment = NULL;
  mailobj->debuglog = NULL;
  for (i = 0; i < 26; i++) {
    mailobj->dtable[i] = (char)('A' + i);
    mailobj->dtable[26 + i] = (char)('a' + i);
  }
  for (i = 0; i < 10; i++) {
    mailobj->dtable[52 + i] = (char)('0' + i);
  }
  mailobj->dtable[62] = '+';
  mailobj->dtable[63] = '/';
  srand(time(NULL));
  return mailobj;
}

DLL_EXPORT_LIBQUICKMAIL void quickmail_destroy (quickmail mailobj)
{
  free(mailobj->from);
  email_info_string_list_free(&mailobj->to);
  email_info_string_list_free(&mailobj->cc);
  email_info_string_list_free(&mailobj->bcc);
  free(mailobj->subject);
  free(mailobj->body);
  email_info_attachment_list_free(&mailobj->attachmentlist);
  free(mailobj->buf);
  free(mailobj->mime_boundary);
  free(mailobj);
}

DLL_EXPORT_LIBQUICKMAIL void quickmail_set_from (quickmail mailobj, const char* from)
{
  free(mailobj->from);
  mailobj->from = strdup(from);
}

DLL_EXPORT_LIBQUICKMAIL void quickmail_add_to (quickmail mailobj, const char* email)
{
  email_info_string_list_add(&mailobj->to, email);
}

DLL_EXPORT_LIBQUICKMAIL void quickmail_add_cc (quickmail mailobj, const char* email)
{
  email_info_string_list_add(&mailobj->cc, email);
}

DLL_EXPORT_LIBQUICKMAIL void quickmail_add_bcc (quickmail mailobj, const char* email)
{
  email_info_string_list_add(&mailobj->bcc, email);
}

DLL_EXPORT_LIBQUICKMAIL void quickmail_set_subject (quickmail mailobj, const char* subject)
{
  free(mailobj->subject);
  mailobj->from = (subject ? strdup(subject) : NULL);
}

DLL_EXPORT_LIBQUICKMAIL void quickmail_set_body (quickmail mailobj, const char* body)
{
  free(mailobj->body);
  mailobj->body = (body ? strdup(body) : NULL);
}

DLL_EXPORT_LIBQUICKMAIL void quickmail_add_attachment_file (quickmail mailobj, const char* path)
{
  email_info_attachment_list_add_file(&mailobj->attachmentlist, path);
}

/*
DLL_EXPORT_LIBQUICKMAIL void quickmail_add_attachment_file (quickmail mailobj, const char* filename, char* data, size_t datalen, int mustfree)
{
  email_info_attachment_list_add_memory(&mailobj->attachmentlist, path);
}
*/

DLL_EXPORT_LIBQUICKMAIL void quickmail_set_debug_log (quickmail mailobj, FILE* filehandle)
{
  mailobj->debuglog = filehandle;
}

DLL_EXPORT_LIBQUICKMAIL void quickmail_fsave (quickmail mailobj, FILE* filehandle)
{
  int i;
  size_t n;
  char buf[80];
  while ((n = quickmail_get_data(buf, sizeof(buf), 1, mailobj)) > 0) {
    for (i = 0; i < n; i++)
      fprintf(filehandle, "%c", buf[i]);
  }
}

DLL_EXPORT_LIBQUICKMAIL size_t quickmail_get_data (void* ptr, size_t size, size_t nmemb, void* userp)
{
  struct email_info_struct* mailobj = (struct email_info_struct*)userp;

  //abort if no data is requested
  if (size * nmemb == 0)
    return 0;

  //initialize on first run
  if (mailobj->current == MAILPART_INITIALIZE) {
    free(mailobj->buf);
    mailobj->buf = NULL;
    mailobj->buflen = 0;
    free(mailobj->mime_boundary);
    mailobj->mime_boundary = (mailobj->attachmentlist ? randomize_zeros(strdup("=SEPARATOR=_0000_0000_0000_0000_0000_0000_=")) : NULL);
    mailobj->current_attachment = mailobj->attachmentlist;
    mailobj->current++;
  }

  //process current part of mail if no partial data is pending
  while (mailobj->buflen == 0) {
    if (mailobj->buflen == 0 && mailobj->current == MAILPART_HEADER) {
      //generate header part
      struct email_info_string_list_struct* listentry;
      char** p = &mailobj->buf;
      //mailobj->buf = NULL;
      str_append(p, "User-Agent: libquickmail v" LIBQUICKMAIL_VERSION);
      if (mailobj->timestamp != 0) {
        char timestamptext[32];
        if (strftime(timestamptext, sizeof(timestamptext), "%a, %d %b %Y %H:%M:%S +0000", gmtime(&mailobj->timestamp))) {
          str_append(p, NEWLINE "Date: ");
          str_append(p, timestamptext);
        }
      }
      if (mailobj->from && *mailobj->from) {
        str_append(p, NEWLINE "From: <");
        str_append(p, mailobj->from);
        str_append(p, ">");
      }
      listentry = mailobj->to;
      while (listentry) {
        if (listentry->data && *listentry->data) {
          str_append(p, NEWLINE "To: <");
          str_append(p, listentry->data);
          str_append(p, ">");
        }
        listentry = listentry->next;
      }
      listentry = mailobj->cc;
      while (listentry) {
        if (listentry->data && *listentry->data) {
          str_append(p, NEWLINE "Cc: <");
          str_append(p, listentry->data);
          str_append(p, ">");
        }
        listentry = listentry->next;
      }
      if (mailobj->subject) {
        str_append(p, NEWLINE "Subject: ");
        str_append(p, mailobj->subject);
      }
      if (mailobj->attachmentlist) {
        str_append(p, NEWLINE "MIME-Version: 1.0" NEWLINE "Content-Type: multipart/mixed; boundary=\"");
        str_append(p, mailobj->mime_boundary);
        str_append(p, "\"" NEWLINE NEWLINE "--");
        str_append(p, mailobj->mime_boundary);
        str_append(p, NEWLINE "Content-Type: text/plain");
      }

      str_append(p, NEWLINE NEWLINE);
      mailobj->buflen = strlen(mailobj->buf);
      mailobj->current++;
    }
    if (mailobj->buflen == 0 && mailobj->current == MAILPART_BODY) {
      if (mailobj->body) {
        //generate body part
        mailobj->buf = mailobj->body;
        mailobj->buflen = strlen(mailobj->buf);
        mailobj->body = NULL;
      } else if (mailobj->current_attachment) {
        if (!mailobj->current_attachment->isopen) {
          //open file to attach
          while (mailobj->current_attachment && mailobj->current_attachment->path) {
            if (mailobj->current_attachment->email_info_attachment_open(mailobj->current_attachment) == 0) {
              mailobj->current_attachment->isopen = 1;
              break;
            }
            mailobj->current_attachment = mailobj->current_attachment->next;
          }
          //generate attachment header
          if (mailobj->current_attachment && mailobj->current_attachment->isopen) {
            //determine base filename
            char* basename = mailobj->current_attachment->path + strlen(mailobj->current_attachment->path);
            while (basename != mailobj->current_attachment->path) {
              basename--;
              if (*basename == '/'
#ifdef __WIN32__
                  || *basename == '\\' || *basename == ':'
#endif
              ) {
                basename++;
                break;
              }
            }
            //generate attachment header
            mailobj->buf = NULL;
            mailobj->buf = str_append(&mailobj->buf, NEWLINE "--");
            mailobj->buf = str_append(&mailobj->buf, mailobj->mime_boundary);
            mailobj->buf = str_append(&mailobj->buf, NEWLINE "Content-Type: application/octet-stream; Name=\"");
            mailobj->buf = str_append(&mailobj->buf, basename);
            mailobj->buf = str_append(&mailobj->buf, "\"" NEWLINE "Content-Transfer-Encoding: base64" NEWLINE NEWLINE);
            mailobj->buflen = strlen(mailobj->buf);
          }
        } else {
          //generate next line of attachment data
          size_t n = 0;
          int mimelinepos = 0;
          unsigned char igroup[3] = {0, 0, 0};
          unsigned char ogroup[4];
          mailobj->buf = (char*)malloc(MIME_LINE_WIDTH + NEWLINELENGTH + 1);
          mailobj->buflen = 0;
          while (mimelinepos < MIME_LINE_WIDTH && (n = mailobj->current_attachment->email_info_attachment_read(mailobj->current_attachment, igroup, 3)) > 0) {
            //code data
            ogroup[0] = mailobj->dtable[igroup[0] >> 2];
            ogroup[1] = mailobj->dtable[((igroup[0] & 3) << 4) | (igroup[1] >> 4)];
            ogroup[2] = mailobj->dtable[((igroup[1] & 0xF) << 2) | (igroup[2] >> 6)];
            ogroup[3] = mailobj->dtable[igroup[2] & 0x3F];
            //padd with "=" characters if less than 3 characters were read
            if (n < 3) {
              ogroup[3] = '=';
              if (n < 2)
                ogroup[2] = '=';
            }
            memcpy(mailobj->buf + mimelinepos, ogroup, 4);
            mailobj->buflen += 4;
            mimelinepos += 4;
          }
          if (mimelinepos > 0) {
            memcpy(mailobj->buf + mimelinepos, NEWLINE, NEWLINELENGTH);
            mailobj->buflen += NEWLINELENGTH;
          }
          if (n <= 0)
          {
            //end of file
            if (mailobj->current_attachment->email_info_attachment_close)
              mailobj->current_attachment->email_info_attachment_close(mailobj->current_attachment);
            mailobj->current_attachment->isopen = 0;
            mailobj->current_attachment = mailobj->current_attachment->next;
          }
        }
      } else {
        mailobj->current++;
      }
    }
    if (mailobj->buflen == 0 && mailobj->current == MAILPART_END) {
      mailobj->buf = NULL;
      mailobj->buflen = 0;
      if (mailobj->attachmentlist) {
        mailobj->buf = str_append(&mailobj->buf, NEWLINE "--");
        mailobj->buf = str_append(&mailobj->buf, mailobj->mime_boundary);
        mailobj->buf = str_append(&mailobj->buf, "--" NEWLINE);
        mailobj->buflen = strlen(mailobj->buf);
      }
      //mailobj->buf = str_append(&mailobj->buf, NEWLINE "." NEWLINE);
      //mailobj->buflen = strlen(mailobj->buf);
      mailobj->current++;
    }
    if (mailobj->buflen == 0 && mailobj->current == MAILPART_DONE) {
      break;
    }
  }

  //flush pending data if any
  if (mailobj->buflen > 0) {
    int len = (mailobj->buflen > size * nmemb ? size * nmemb : mailobj->buflen);
    memcpy(ptr, mailobj->buf, len);
    if (len < mailobj->buflen) {
      mailobj->buf = memmove(mailobj->buf, mailobj->buf + len, mailobj->buflen - len);
      mailobj->buflen -= len;
    } else {
      free(mailobj->buf);
      mailobj->buf = NULL;
      mailobj->buflen = 0;
    }
    return len;
  }

  //if (mailobj->current != MAILPART_DONE)
  //  ;//this should never be reached
  mailobj->current = 0;
  return 0;
}

DLL_EXPORT_LIBQUICKMAIL const char* quickmail_send (quickmail mailobj, const char* smtpserver, unsigned int smtpport, const char* username, const char* password)
{
#ifndef NOCURL
  //libcurl based sending
  CURL *curl;
  CURLcode result = CURLE_FAILED_INIT;
  //curl_global_init(CURL_GLOBAL_ALL);
  if ((curl = curl_easy_init()) != NULL) {
    struct curl_slist *recipients = NULL;
    struct email_info_string_list_struct* listentry;
    //set destination URL
    size_t l = strlen(smtpserver) + 14;
    char* url = (char*)malloc(l);
    snprintf(url, l, "smtp://%s:%u", smtpserver, smtpport);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    free(url);
    //try Transport Layer Security (TLS), but continue anyway if it fails
    curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_TRY);
    //don't fail if the TLS/SSL a certificate could not be verified
    //alternative: add the issuer certificate (or the host certificate if
    //the certificate is self-signed) to the set of certificates that are
    //known to libcurl using CURLOPT_CAINFO and/or CURLOPT_CAPATH
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    //set authentication credentials if provided
    if (username && *username)
      curl_easy_setopt(curl, CURLOPT_USERNAME, username);
    if (password)
      curl_easy_setopt(curl, CURLOPT_PASSWORD, password);
    //set from value for envelope reverse-path
    if (mailobj->from && *mailobj->from)
      curl_easy_setopt(curl, CURLOPT_MAIL_FROM, mailobj->from);
    //set recipients
    listentry = mailobj->to;
    while (listentry) {
      if (listentry->data && *listentry->data)
        recipients = curl_slist_append(recipients, listentry->data);
      listentry = listentry->next;
    }
    listentry = mailobj->cc;
    while (listentry) {
      if (listentry->data && *listentry->data)
        recipients = curl_slist_append(recipients, listentry->data);
      listentry = listentry->next;
    }
    listentry = mailobj->bcc;
    while (listentry) {
      if (listentry->data && *listentry->data)
        recipients = curl_slist_append(recipients, listentry->data);
      listentry = listentry->next;
    }
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    //set callback function for getting message body
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, quickmail_get_data);
    curl_easy_setopt(curl, CURLOPT_READDATA, mailobj);
    //enable debugging if requested
    if (mailobj->debuglog) {
      curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
      curl_easy_setopt(curl, CURLOPT_STDERR, mailobj->debuglog);
    }
    //send the message
    result = curl_easy_perform(curl);
    //free the list of recipients and clean up
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);
  }
  return (result == CURLE_OK ? NULL : curl_easy_strerror(result));
#else
  //minimal implementation without libcurl
  SOCKET sock;
  char* errmsg = NULL;
  struct email_info_string_list_struct* listentry;
  char local_hostname[64];
  int statuscode;
  //determine local host name
  if (gethostname(local_hostname, sizeof(local_hostname)) != 0)
		strcpy(local_hostname, "localhost");
  //connect
  if ((sock = socket_open(smtpserver, smtpport, &errmsg)) != INVALID_SOCKET) {
    //talk with SMTP server
    if ((statuscode = socket_smtp_command(sock, mailobj->debuglog, NULL)) >= 400) {
      errmsg = "SMTP server returned an error on connection";
    } else {
      do {
        if ((statuscode = socket_smtp_command(sock, mailobj->debuglog, "EHLO %s", local_hostname)) >= 400) {
          if ((statuscode = socket_smtp_command(sock, mailobj->debuglog, "HELO %s", local_hostname)) >= 400) {
            errmsg = "SMTP EHLO/HELO returned error";
            break;
          }
        }
        //authenticate if needed
        if (username || password) {
          int len;
          int inpos = 0;
          int outpos = 0;
          size_t usernamelen = (username ? strlen(username) : 0);
          size_t passwordlen = (password ? strlen(password) : 0);
          char* auth = (char*)malloc(usernamelen + passwordlen + 4);
          char* base64auth = (char*)malloc(((usernamelen + passwordlen + 2) + 2) / 3 * 4 + 1);
          //leave the authorization identity empty to indicate it's the same the as authentication identity
          auth[0] = 0;
          len = 1;
          //set the authentication identity
          memcpy(auth + len, (username ? username : ""), usernamelen + 1);
          len += usernamelen + 1;
          //set the password
          memcpy(auth + len, (password ? password : ""), passwordlen + 1);
          len += passwordlen;
          //padd with extra zero so groups of 3 bytes can be read
          auth[usernamelen + len + 1] = 0;
          //encode in base64
          while (inpos < len) {
            //encode data
            base64auth[outpos + 0] = mailobj->dtable[auth[inpos + 0] >> 2];
            base64auth[outpos + 1] = mailobj->dtable[((auth[inpos + 0] & 3) << 4) | (auth[inpos + 1] >> 4)];
            base64auth[outpos + 2] = mailobj->dtable[((auth[inpos + 1] & 0xF) << 2) | (auth[inpos + 2] >> 6)];
            base64auth[outpos + 3] = mailobj->dtable[auth[inpos + 2] & 0x3F];
            //padd with "=" characters if less than 3 characters were read
            if (inpos + 1 >= len) {
              base64auth[outpos + 3] = '=';
              if (inpos + 2 >= len)
                base64auth[outpos + 2] = '=';
            }
            //advance to next position
            inpos += 3;
            outpos += 4;
          }
          base64auth[outpos] = 0;
          //send originator e-mail address
          if ((statuscode = socket_smtp_command(sock, mailobj->debuglog, "AUTH PLAIN %s", base64auth)) >= 400) {
            errmsg = "SMTP authentication failed";
            break;
          }
        }
        //send originator e-mail address
        if ((statuscode = socket_smtp_command(sock, mailobj->debuglog, "MAIL FROM:<%s>", mailobj->from)) >= 400) {
          errmsg = "SMTP server did not accept sender";
          break;
        }
        //send recipient e-mail addresses
        listentry = mailobj->to;
        while (!errmsg && listentry) {
          if (listentry->data && *listentry->data) {
            if ((statuscode = socket_smtp_command(sock, mailobj->debuglog, "RCPT TO:<%s>", listentry->data)) >= 400)
              errmsg = "SMTP server did not accept e-mail address (To)";
          }
          listentry = listentry->next;
        }
        listentry = mailobj->cc;
        while (!errmsg && listentry) {
          if (listentry->data && *listentry->data) {
            if ((statuscode = socket_smtp_command(sock, mailobj->debuglog, "RCPT TO:<%s>", listentry->data)) >= 400)
              errmsg = "SMTP server did not accept e-mail address (CC)";
          }
          listentry = listentry->next;
        }
        listentry = mailobj->bcc;
        while (!errmsg && listentry) {
          if (listentry->data && *listentry->data) {
            if ((statuscode = socket_smtp_command(sock, mailobj->debuglog, "RCPT TO:<%s>", listentry->data)) >= 400)
              errmsg = "SMTP server did not accept e-mail address (BCC)";
          }
          listentry = listentry->next;
        }
        if (errmsg)
          break;
        //prepare to send mail body
        if ((statuscode = socket_smtp_command(sock, mailobj->debuglog, "DATA")) >= 400) {
          errmsg = "SMTP DATA returned error";
          break;
        }
        //send mail body data
        size_t n;
        char buf[WRITE_BUFFER_CHUNK_SIZE];
        while ((n = quickmail_get_data(buf, sizeof(buf), 1, mailobj)) > 0) {
          socket_send(sock, buf, n);
        }
        //send end of data
        if ((statuscode = socket_smtp_command(sock, mailobj->debuglog, "\r\n.")) >= 400) {
          errmsg = "SMTP error after sending message data";
          break;
        }
      } while (0);
      //log out
      socket_smtp_command(sock, mailobj->debuglog, "QUIT");
    }
  }
  //close socket
  socket_close(sock);
  return errmsg;
#endif
}
