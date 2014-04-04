// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: cachedb.cc,v 1.7 2004/05/08 19:41:01 mdz Exp $
/* ######################################################################

   CacheDB
   
   Simple uniform interface to a cache database.
   
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include <config.h>

#include <apt-pkg/error.h>
#include <apt-pkg/md5.h>
#include <apt-pkg/sha1.h>
#include <apt-pkg/sha2.h>
#include <apt-pkg/strutl.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/fileutl.h>
#include <apt-pkg/debfile.h>
#include <apt-pkg/gpgv.h>

#include <netinet/in.h>       // htonl, etc
#include <ctype.h>
#include <stddef.h>
#include <sys/stat.h>

#include "cachedb.h"

#include <apti18n.h>
									/*}}}*/

// CacheDB::ReadyDB - Ready the DB2					/*{{{*/
// ---------------------------------------------------------------------
/* This opens the DB2 file for caching package information */
bool CacheDB::ReadyDB(std::string const &DB)
{
   int err;

   ReadOnly = _config->FindB("APT::FTPArchive::ReadOnlyDB",false);
   
   // Close the old DB
   if (Dbp != 0) 
      Dbp->close(Dbp,0);
   
   /* Check if the DB was disabled while running and deal with a 
      corrupted DB */
   if (DBFailed() == true)
   {
      _error->Warning(_("DB was corrupted, file renamed to %s.old"),DBFile.c_str());
      rename(DBFile.c_str(),(DBFile+".old").c_str());
   }
   
   DBLoaded = false;
   Dbp = 0;
   DBFile = std::string();
   
   if (DB.empty())
      return true;

   db_create(&Dbp, NULL, 0);
   if ((err = Dbp->open(Dbp, NULL, DB.c_str(), NULL, DB_BTREE,
                        (ReadOnly?DB_RDONLY:DB_CREATE),
                        0644)) != 0)
   {
      if (err == DB_OLD_VERSION)
      {
          _error->Warning(_("DB is old, attempting to upgrade %s"),DBFile.c_str());
	  err = Dbp->upgrade(Dbp, DB.c_str(), 0);
	  if (!err)
	     err = Dbp->open(Dbp, NULL, DB.c_str(), NULL, DB_HASH,
                            (ReadOnly?DB_RDONLY:DB_CREATE), 0644);

      }
      // the database format has changed from DB_HASH to DB_BTREE in 
      // apt 0.6.44
      if (err == EINVAL)
      {
	 _error->Error(_("DB format is invalid. If you upgraded from an older version of apt, please remove and re-create the database."));
      }
      if (err)
      {
          Dbp = 0;
          return _error->Error(_("Unable to open DB file %s: %s"),DB.c_str(), db_strerror(err));
      }
   }
   
   DBFile = DB;
   DBLoaded = true;
   return true;
}
									/*}}}*/
// CacheDB::OpenFile - Open the file					/*{{{*/
// ---------------------------------------------------------------------
/* */
bool CacheDB::OpenFile()
{
   // its open already
   if(Fd && Fd->Name() == this->FileName)
      return true;

   // a different file is open, close it first
   if(Fd && Fd->Name() != this->FileName)
      CloseFile();

   // open a new file
   Fd = new FileFd(FileName,FileFd::ReadOnly);
   if (_error->PendingError() == true)
   {
      CloseFile();
      return false;
   }
   return true;
}
									/*}}}*/
// CacheDB::CloseFile - Close the file					/*{{{*/
void CacheDB::CloseFile()
{
   if(Fd != NULL)
   {
      delete Fd;
      Fd = NULL;
   }
}
									/*}}}*/
// CacheDB::OpenDebFile - Open a debfile				/*{{{*/
bool CacheDB::OpenDebFile()
{
   // debfile is already open
   if(DebFile && &DebFile->GetFile() == Fd)
      return true;

   // a different debfile is open, close it first
   if(DebFile && &DebFile->GetFile() != Fd)
      CloseDebFile();

   // first open the fd, then pass it to the debDebFile
   if(OpenFile() == false)
      return false;
   DebFile = new debDebFile(*Fd);
   if (_error->PendingError() == true)
      return false;
   return true;
}
									/*}}}*/
// CacheDB::CloseDebFile - Close a debfile again 			/*{{{*/
void CacheDB::CloseDebFile()
{
   CloseFile();

   if(DebFile != NULL)
   {
      delete DebFile;
      DebFile = NULL;
   }
}
									/*}}}*/
// CacheDB::GetFileStat - Get stats from the file 			/*{{{*/
// ---------------------------------------------------------------------
/* This gets the size from the database if it's there.  If we need
 * to look at the file, also get the mtime from the file. */
bool CacheDB::GetFileStat(bool const &doStat)
{
   if ((CurStat.Flags & FlSize) == FlSize && doStat == false)
      return true;

   /* Get it from the file. */
   if (OpenFile() == false)
      return false;
   
   // Stat the file
   struct stat St;
   if (fstat(Fd->Fd(),&St) != 0)
   {
      CloseFile();
      return _error->Errno("fstat",
                           _("Failed to stat %s"),FileName.c_str());
   }
   CurStat.FileSize = St.st_size;
   CurStat.mtime = htonl(St.st_mtime);
   CurStat.Flags |= FlSize;
   
   return true;
}
									/*}}}*/
// CacheDB::GetCurStat - Set the CurStat variable.			/*{{{*/
// ---------------------------------------------------------------------
/* Sets the CurStat variable.  Either to 0 if no database is used
 * or to the value in the database if one is used */
bool CacheDB::GetCurStat()
{
   memset(&CurStat,0,sizeof(CurStat));
   
   if (DBLoaded)
   {
      /* First see if there is anything about it
         in the database */
      
      /* Get the flags (and mtime) */
      InitQueryStats();
      // Ensure alignment of the returned structure
      Data.data = &CurStat;
      Data.ulen = sizeof(CurStat);
      Data.flags = DB_DBT_USERMEM;
      if (Get() == false)
      {
	 CurStat.Flags = 0;
      }      
      CurStat.Flags = ntohl(CurStat.Flags);
      CurStat.FileSize = ntohl(CurStat.FileSize);
   }      
   return true;
}
									/*}}}*/
// CacheDB::GetFileInfo - Get all the info about the file		/*{{{*/
// ---------------------------------------------------------------------
bool CacheDB::GetFileInfo(std::string const &FileName, bool const &DoControl, 
                          bool const &DoContents,
                          bool const &GenContentsOnly, 
                          bool const &DoSource,
                          bool const &DoMD5, bool const &DoSHA1,
                          bool const &DoSHA256, bool const &DoSHA512, 
                          bool const &checkMtime)
{
   bool result = true;
   this->FileName = FileName;

   if (GetCurStat() == false)
      return false;
   OldStat = CurStat;

   if (GetFileStat(checkMtime) == false)
      return false;	

   /* if mtime changed, update CurStat from disk */
   if (checkMtime == true && OldStat.mtime != CurStat.mtime)
      CurStat.Flags = FlSize;

   Stats.Bytes += CurStat.FileSize;
   Stats.Packages++;

   if ((DoControl && LoadControl() == false)
       || (DoContents && LoadContents(GenContentsOnly) == false)
       || (DoSource && LoadSource() == false)
       || (DoMD5 && GetMD5(false) == false)
       || (DoSHA1 && GetSHA1(false) == false)
       || (DoSHA256 && GetSHA256(false) == false)
       || (DoSHA512 && GetSHA512(false) == false) )
   {
      result = false;
   }
    
   return result;
}
									/*}}}*/

bool CacheDB::LoadSource()
{
   // Try to read the control information out of the DB.
   if ((CurStat.Flags & FlSource) == FlSource)
   {
      // Lookup the control information
      InitQuerySource();
      if (Get() == true && Dsc.TakeDsc(Data.data, Data.size) == true)
      {
	    return true;
      }
      CurStat.Flags &= ~FlSource;
   }
   if (OpenFile() == false)
      return false;

   Stats.Misses++;
   if (Dsc.Read(FileName) == false)
      return false;

   if (Dsc.Data == 0)
      return _error->Error(_("Failed to read .dsc"));
   
   // Write back the control information
   InitQuerySource();
   if (Put(Dsc.Data, Dsc.Length) == true)
      CurStat.Flags |= FlSource;

   return true;
}

// CacheDB::LoadControl - Load Control information			/*{{{*/
// ---------------------------------------------------------------------
/* */
bool CacheDB::LoadControl()
{
   // Try to read the control information out of the DB.
   if ((CurStat.Flags & FlControl) == FlControl)
   {
      // Lookup the control information
      InitQueryControl();
      if (Get() == true && Control.TakeControl(Data.data,Data.size) == true)
	    return true;
      CurStat.Flags &= ~FlControl;
   }
   
   if(OpenDebFile() == false)
      return false;
   
   Stats.Misses++;
   if (Control.Read(*DebFile) == false)
      return false;

   if (Control.Control == 0)
      return _error->Error(_("Archive has no control record"));
   
   // Write back the control information
   InitQueryControl();
   if (Put(Control.Control,Control.Length) == true)
      CurStat.Flags |= FlControl;
   return true;
}
									/*}}}*/
// CacheDB::LoadContents - Load the File Listing			/*{{{*/
// ---------------------------------------------------------------------
/* */
bool CacheDB::LoadContents(bool const &GenOnly)
{
   // Try to read the control information out of the DB.
   if ((CurStat.Flags & FlContents) == FlContents)
   {
      if (GenOnly == true)
	 return true;
      
      // Lookup the contents information
      InitQueryContent();
      if (Get() == true)
      {
	 if (Contents.TakeContents(Data.data,Data.size) == true)
	    return true;
      }
      
      CurStat.Flags &= ~FlContents;
   }
   
   if(OpenDebFile() == false)
      return false;

   Stats.Misses++;
   if (Contents.Read(*DebFile) == false)
      return false;	    
   
   // Write back the control information
   InitQueryContent();
   if (Put(Contents.Data,Contents.CurSize) == true)
      CurStat.Flags |= FlContents;
   return true;
}
									/*}}}*/

static std::string bytes2hex(uint8_t *bytes, size_t length) {
   char buf[3];
   std::string space;

   space.reserve(length*2 + 1);
   for (size_t i = 0; i < length; i++) {
      snprintf(buf, sizeof(buf), "%02x", bytes[i]);
      space.append(buf);
   }
   return space;
}

static inline unsigned char xdig2num(char const &dig) {
   if (isdigit(dig)) return dig - '0';
   if ('a' <= dig && dig <= 'f') return dig - 'a' + 10;
   if ('A' <= dig && dig <= 'F') return dig - 'A' + 10;
   return 0;
}

static void hex2bytes(uint8_t *bytes, const char *hex, int length) {
   while (length-- > 0) {
      *bytes = 0;
      if (isxdigit(hex[0]) && isxdigit(hex[1])) {
	  *bytes = xdig2num(hex[0]) * 16 + xdig2num(hex[1]);
	  hex += 2;
      }
      bytes++;
   } 
}

// CacheDB::GetMD5 - Get the MD5 hash					/*{{{*/
// ---------------------------------------------------------------------
/* */
bool CacheDB::GetMD5(bool const &GenOnly)
{
   // Try to read the control information out of the DB.
   if ((CurStat.Flags & FlMD5) == FlMD5)
   {
      if (GenOnly == true)
	 return true;
      
      MD5Res = bytes2hex(CurStat.MD5, sizeof(CurStat.MD5));
	 return true;
   }
   
   Stats.MD5Bytes += CurStat.FileSize;
	 
   if (OpenFile() == false)
      return false;

   MD5Summation MD5;
   if (Fd->Seek(0) == false || MD5.AddFD(*Fd, CurStat.FileSize) == false)
      return false;
   
   MD5Res = MD5.Result();
   hex2bytes(CurStat.MD5, MD5Res.data(), sizeof(CurStat.MD5));
      CurStat.Flags |= FlMD5;
   return true;
}
									/*}}}*/
// CacheDB::GetSHA1 - Get the SHA1 hash					/*{{{*/
// ---------------------------------------------------------------------
/* */
bool CacheDB::GetSHA1(bool const &GenOnly)
{
   // Try to read the control information out of the DB.
   if ((CurStat.Flags & FlSHA1) == FlSHA1)
   {
      if (GenOnly == true)
	 return true;

      SHA1Res = bytes2hex(CurStat.SHA1, sizeof(CurStat.SHA1));
      return true;
   }
   
   Stats.SHA1Bytes += CurStat.FileSize;
	 
   if (OpenFile() == false)
      return false;

   SHA1Summation SHA1;
   if (Fd->Seek(0) == false || SHA1.AddFD(*Fd, CurStat.FileSize) == false)
      return false;
   
   SHA1Res = SHA1.Result();
   hex2bytes(CurStat.SHA1, SHA1Res.data(), sizeof(CurStat.SHA1));
   CurStat.Flags |= FlSHA1;
   return true;
}
									/*}}}*/
// CacheDB::GetSHA256 - Get the SHA256 hash				/*{{{*/
// ---------------------------------------------------------------------
/* */
bool CacheDB::GetSHA256(bool const &GenOnly)
{
   // Try to read the control information out of the DB.
   if ((CurStat.Flags & FlSHA256) == FlSHA256)
   {
      if (GenOnly == true)
	 return true;

      SHA256Res = bytes2hex(CurStat.SHA256, sizeof(CurStat.SHA256));
      return true;
   }
   
   Stats.SHA256Bytes += CurStat.FileSize;
	 
   if (OpenFile() == false)
      return false;

   SHA256Summation SHA256;
   if (Fd->Seek(0) == false || SHA256.AddFD(*Fd, CurStat.FileSize) == false)
      return false;
   
   SHA256Res = SHA256.Result();
   hex2bytes(CurStat.SHA256, SHA256Res.data(), sizeof(CurStat.SHA256));
   CurStat.Flags |= FlSHA256;
   return true;
}
									/*}}}*/
// CacheDB::GetSHA256 - Get the SHA256 hash				/*{{{*/
// ---------------------------------------------------------------------
/* */
bool CacheDB::GetSHA512(bool const &GenOnly)
{
   // Try to read the control information out of the DB.
   if ((CurStat.Flags & FlSHA512) == FlSHA512)
   {
      if (GenOnly == true)
	 return true;

      SHA512Res = bytes2hex(CurStat.SHA512, sizeof(CurStat.SHA512));
      return true;
   }
   
   Stats.SHA512Bytes += CurStat.FileSize;
	 
   if (OpenFile() == false)
      return false;

   SHA512Summation SHA512;
   if (Fd->Seek(0) == false || SHA512.AddFD(*Fd, CurStat.FileSize) == false)
      return false;
   
   SHA512Res = SHA512.Result();
   hex2bytes(CurStat.SHA512, SHA512Res.data(), sizeof(CurStat.SHA512));
   CurStat.Flags |= FlSHA512;
   return true;
}
									/*}}}*/
// CacheDB::Finish - Write back the cache structure			/*{{{*/
// ---------------------------------------------------------------------
/* */
bool CacheDB::Finish()
{
   // Optimize away some writes.
   if (CurStat.Flags == OldStat.Flags &&
       CurStat.mtime == OldStat.mtime)
      return true;
   
   // Write the stat information
   CurStat.Flags = htonl(CurStat.Flags);
   CurStat.FileSize = htonl(CurStat.FileSize);
   InitQueryStats();
   Put(&CurStat,sizeof(CurStat));
   CurStat.Flags = ntohl(CurStat.Flags);
   CurStat.FileSize = ntohl(CurStat.FileSize);

   return true;
}
									/*}}}*/
// CacheDB::Clean - Clean the Database					/*{{{*/
// ---------------------------------------------------------------------
/* Tidy the database by removing files that no longer exist at all. */
bool CacheDB::Clean()
{
   if (DBLoaded == false)
      return true;

   /* I'm not sure what VERSION_MINOR should be here.. 2.4.14 certainly
      needs the lower one and 2.7.7 needs the upper.. */
   DBC *Cursor;
   if ((errno = Dbp->cursor(Dbp, NULL, &Cursor, 0)) != 0)
      return _error->Error(_("Unable to get a cursor"));
   
   DBT Key;
   DBT Data;
   memset(&Key,0,sizeof(Key));
   memset(&Data,0,sizeof(Data));
   while ((errno = Cursor->c_get(Cursor,&Key,&Data,DB_NEXT)) == 0)
   {
      const char *Colon = (char*)memrchr(Key.data, ':', Key.size);
      if (Colon)
      {
         if (stringcmp(Colon + 1, (char *)Key.data+Key.size,"st") == 0 ||
             stringcmp(Colon + 1, (char *)Key.data+Key.size,"cl") == 0 ||
             stringcmp(Colon + 1, (char *)Key.data+Key.size,"cs") == 0 ||
             stringcmp(Colon + 1, (char *)Key.data+Key.size,"cn") == 0)
	 {
            std::string FileName = std::string((const char *)Key.data,Colon);
            if (FileExists(FileName) == true) {
		continue;
            }
	 }
      }
      Cursor->c_del(Cursor,0);
   }
   int res = Dbp->compact(Dbp, NULL, NULL, NULL, NULL, DB_FREE_SPACE, NULL);
   if (res < 0)
      _error->Warning("compact failed with result %i", res);

   if(_config->FindB("Debug::APT::FTPArchive::Clean", false) == true)
      Dbp->stat_print(Dbp, 0);


   return true;
}
									/*}}}*/
