#include <cassert>
#include <time.h>

#if defined(HAVE_CONFIG_H)
  #include "config.h"
#endif

#include "resip/dum/RegistrationPersistenceManager.hxx"
#include "resip/stack/Symbols.hxx"
#include "resip/stack/Tuple.hxx"
#include "resip/stack/SipStack.hxx"
#include "rutil/Data.hxx"
#include "rutil/DnsUtil.hxx"
#include "rutil/Logger.hxx"
#include "rutil/MD5Stream.hxx"
#include "rutil/ParseBuffer.hxx"
#include "rutil/Socket.hxx"
#include "rutil/Timer.hxx"
#include "rutil/TransportType.hxx"

#include "repro/ReproVersion.hxx"
#include "repro/Proxy.hxx"
#include "repro/HttpBase.hxx"
#include "repro/HttpConnection.hxx"
#include "repro/WebAdmin.hxx"
#include "repro/RouteStore.hxx"
#include "repro/UserStore.hxx"
#include "repro/FilterStore.hxx"
#include "repro/Store.hxx"

#ifdef USE_SSL
#include "resip/stack/ssl/Security.hxx"
#endif

using namespace resip;
using namespace repro;
using namespace std;

#define RESIPROCATE_SUBSYSTEM Subsystem::REPRO

#define REPRO_BORDERLESS_TABLE_PROPS " border=\"0\" cellspacing=\"2\" cellpadding=\"0\""
#define REPRO_BORDERED_TABLE_PROPS " border=\"1\" cellspacing=\"1\" cellpadding=\"1\" bgcolor=\"#ffffff\""

WebAdmin::RemoveKey::RemoveKey(const Data &key1, const Data &key2) : mKey1(key1), mKey2(key2) 
{
}; 

bool
WebAdmin::RemoveKey::operator<(const RemoveKey& rhs) const
{
   if(mKey1 < rhs.mKey1) 
   {
      return true;
   }
   else if(mKey1 == rhs.mKey1 && mKey2 < rhs.mKey2) 
   { 
      return true; 
   }
   else 
   {
      return false;
   }
}

WebAdmin::WebAdmin(Proxy& proxy,
                   RegistrationPersistenceManager& regDb,
                   const Data& realm, // this realm is used for http challenges                
                   int port,
                   IpVersion version,
                   const Data& ipAddr):
   HttpBase(port, version, realm, ipAddr),
   mProxy(proxy),
   mStore(*mProxy.getConfig().getDataStore()),
   mRegDb(regDb),
   mNoWebChallenges(proxy.getConfig().getConfigBool("DisableHttpAuth", false)),
   mPageOutlinePre(
#include "repro/webadmin/pageOutlinePre.ixx"
   ),
   mPageOutlinePost(
#include "repro/webadmin/pageOutlinePost.ixx"
   )
{
   const Data adminName("admin");
   const Data adminPassword= mProxy.getConfig().getConfigData("HttpAdminPassword", "admin");

   // Place repro version into PageOutlinePre
   mPageOutlinePre.replace("VERSION", VersionUtils::instance().releaseVersion().c_str());

   Data dbA1 = mStore.mUserStore.getUserAuthInfo( adminName, Data::Empty );
      
   DebugLog(<< " Looking to see if admin user exists (creating WebAdmin)");
   if ( dbA1.empty() ) // if the admin user does not exist, add it 
   { 
      DebugLog(<< "Creating admin user" );
         
      mStore.mUserStore.addUser(adminName, // user
                                Data::Empty, // domain 
                                Data::Empty, // realm 
                                (adminPassword == "" ? Data("admin") : adminPassword), // password 
                                 true,        // applyA1HashToPassword
                                Data::Empty, // name 
                                Data::Empty ); // email 
      dbA1 = mStore.mUserStore.getUserAuthInfo( adminName, Data::Empty );
      assert(!dbA1.empty());
   }
   else if (adminPassword!=Data(""))
   {
      //All we're using for admin is the password.
      //This next bit of code relies on it being ok that we 
      //blow away any other information
      //in that row. It also expects addUser to replace anything matching the existing key
      DebugLog(<< "Changing the web admin password" );
      mStore.mUserStore.addUser(adminName,
                                Data::Empty,
                                Data::Empty,
                                adminPassword,
                                true,        // applyA1HashToPassword
                                Data::Empty,
                                Data::Empty);
   }
}


void 
WebAdmin::buildPage( const Data& uri,
                     int pageNumber, 
                     const resip::Data& pUser,
                     const resip::Data& pPassword )
{
   ParseBuffer pb(uri);
   
   DebugLog (<< "Parsing URL" << uri );

   const char* anchor = pb.skipChar('/');
   pb.skipToChar('?');
   Data pageName;
   pb.data(pageName,anchor);
   
   DebugLog (<< "  got page name: " << pageName );

   // if this is not a valid page, redirect it
   if (
      ( pageName != Data("index.html") ) && 
      ( pageName != Data("input") ) && 
      ( pageName != Data("cert.cer") ) && 
      ( ! pageName.prefix("cert") ) && 
      ( pageName != Data("userTest.html") ) && 
      ( pageName != Data("domains.html")  ) &&
      ( pageName != Data("acls.html")  ) &&
      ( pageName != Data("addUser.html") ) && 
      ( pageName != Data("editUser.html") ) &&
      ( pageName != Data("showUsers.html")  ) &&
      ( pageName != Data("addFilter.html") ) && 
      ( pageName != Data("editFilter.html") ) &&
      ( pageName != Data("showFilters.html") )&& 
      ( pageName != Data("addRoute.html") ) && 
      ( pageName != Data("editRoute.html") ) &&
      ( pageName != Data("showRoutes.html") )&& 
      ( pageName != Data("registrations.html") ) &&  
      ( pageName != Data("settings.html") ) &&  
      ( pageName != Data("restart.html") ) &&  
      ( pageName != Data("user.html")  ) )
   { 
      setPage( resip::Data::Empty, pageNumber, 301 );
      return; 
   }
   
   // pages anyone can use 
   if ( pageName == Data("index.html") ) 
   {
      setPage( buildDefaultPage(), pageNumber, 200); 
      return;
   }

   // certificate pages 
   if ( pageName.prefix("cert") || pageName == Data("cert.cer") )
   {
#ifdef USE_SSL
      Data domain = mRealm;
      try 
      {
         const char* anchor = pb.skipChar('?');
         pb.skipToChar('=');
         Data query;
         pb.data(query, anchor);
         InfoLog( << "query is " << query );
         if ( query == "domain" ) 
         {
           anchor = pb.skipChar('=');
           pb.skipToEnd();
           pb.data(domain, anchor);
         }
      }
      catch (ParseException& )
      {
      }

      if ( !domain.empty() )
      {
         InfoLog( << "domain is " << domain );
         try
         {
            setPage( buildCertPage(domain), pageNumber, 200, Mime("application","pkix-cert") );
         }
         catch(BaseSecurity::Exception&)
         {
            setPage( resip::Data::Empty, pageNumber, 404 );
         }
         return;
      }
      else
      {
         setPage( resip::Data::Empty, pageNumber, 404 );
         return;
      }
#else
      // ?bwc? Probably could use a better indication?
      setPage(resip::Data::Empty, pageNumber, 404);
#endif
   }
  
   Data authenticatedUser;
   if (mNoWebChallenges)
   {
      // do't do authentication - give everyone admin privilages
      authenticatedUser = Data("admin");
   }
   else
   {
      // TODO !cj! - this code is broken - the user name in the web digest should be
      // moved to alice@example.com instead of alice and assuming the realm is
      // empty

      // all pages after this, user must authenticate  
      if ( pUser.empty() )
      {  
         setPage( resip::Data::Empty, pageNumber,401 );
         return;
      }
      
      // check that authentication is correct 
      Data dbA1 = mStore.mUserStore.getUserAuthInfo( pUser, Data::Empty );
      
#if 0
      if ( dbA1.empty() ) // if the admin user does not exist, add it 
      { 
         mStore.mUserStore.addUser( pUser, // user
                          Data::Empty, // domain 
                          Data::Empty, // realm 
                          Data("admin"), // password 
                          Data::Empty, // name 
                          Data::Empty ); // email 
         dbA1 = mStore.mUserStore.getUserAuthInfo( pUser, Data::Empty );
         assert( !dbA1.empty() );
      }
#endif

      if ( !dbA1.empty() )
      {
         MD5Stream a1;
         a1 << pUser // username
            << Symbols::COLON
            << Data::Empty // realm
            << Symbols::COLON
            << pPassword;
         Data compA1 = a1.getHex();
         
         if ( dbA1 == compA1 )
         {
            authenticatedUser = pUser;
         }
         else
         {
            InfoLog(  << "user " << pUser << " failed to authenticate to web server" );
            DebugLog( << " compA1="<<compA1<< " dbA1="<<dbA1 );
            setPage( resip::Data::Empty, pageNumber,401 );
            return;
         }
      }
      else //No A1, so we must assume this user does not exist.
      {
         setPage( "User does not exist.", pageNumber,401 );
         return;         
      }
   }
      
   // parse any URI tags from form entry
   mRemoveSet.clear();
   mHttpParams.clear();

   if (!pb.eof())
   {
      pb.skipChar('?');
           
      while ( !pb.eof() )
      {
         const char* anchor1 = pb.position();
         pb.skipToChar('=');
         Data key;
         pb.data(key,anchor1);
 
         const char* anchor2 = pb.skipChar('=');
         pb.skipToChar('&');
         Data value;
         pb.data(value,anchor2); 
           
         if ( !pb.eof() )
         {
            pb.skipChar('&');
         }
           
         if ( key.prefix("remove.") )  // special case of parameters to delete one or more records
         {
            Data tmp = key.substr(7);  // the ID is everything after the dot
            if (!tmp.empty())
            {
               DebugLog (<< "  remove key=" << tmp.urlDecoded());
               mRemoveSet.insert(RemoveKey(tmp.urlDecoded(),value.urlDecoded()));   // add to the set of records to remove
            }
         }
         else if ( !key.empty() && !value.empty() ) // make sure both exist
         {
            DebugLog (<< "  key=" << key << " value=" << value << " & unencoded form: " << value.urlDecoded() );
            mHttpParams[key] = value.urlDecoded();  // add other parameters to the Map
         }
      }
   }
   
   DebugLog( << "building page for user=" << authenticatedUser  );

   Data page;
   if ( authenticatedUser == Data("admin") )
   {
      DataStream s(page);
      s << mPageOutlinePre;
      
      // admin only pages 
      if ( pageName == Data("user.html")    ) {}; /* do nothing */ 
      //if ( pageName == Data("input")    ) ; /* do nothing */ 
      if ( pageName == Data("domains.html")    ) buildDomainsSubPage(s);
      if ( pageName == Data("acls.html")       ) buildAclsSubPage(s);
      
      if ( pageName == Data("addUser.html")    ) buildAddUserSubPage(s);
      if ( pageName == Data("editUser.html")   ) buildEditUserSubPage(s);
      if ( pageName == Data("showUsers.html")  ) buildShowUsersSubPage(s);
      
      if ( pageName == Data("addFilter.html")   ) buildAddFilterSubPage(s);
      if ( pageName == Data("editFilter.html")  ) buildEditFilterSubPage(s);
      if ( pageName == Data("showFilters.html") ) buildShowFiltersSubPage(s);

      if ( pageName == Data("addRoute.html")   ) buildAddRouteSubPage(s);
      if ( pageName == Data("editRoute.html")  ) buildEditRouteSubPage(s);
      if ( pageName == Data("showRoutes.html") ) buildShowRoutesSubPage(s);
      
      if ( pageName == Data("registrations.html")) buildRegistrationsSubPage(s);
      if ( pageName == Data("settings.html"))    buildSettingsSubPage(s);
      if ( pageName == Data("restart.html"))     buildRestartSubPage(s);
      
      s << mPageOutlinePost;
      s.flush();

      if ( pageName == Data("userTest.html")   ) page=buildUserPage();
   }
   else if ( !authenticatedUser.empty() )
   {
      // user only pages 
      if ( pageName == Data("user.html") ) page=buildUserPage(); 
      //if ( pageName == Data("input") ) page=buildUserPage();
  }
   
   assert( !authenticatedUser.empty() );
   assert( !page.empty() );
   
   setPage( page, pageNumber,200 );
}


void
WebAdmin::buildDomainsSubPage(DataStream& s)
{ 
   Data domainUri;
   int domainTlsPort;

  if (!mRemoveSet.empty() && (mHttpParams["action"] == "Remove"))
   {
      int j = 0;
      for (set<RemoveKey>::iterator i = mRemoveSet.begin(); i != mRemoveSet.end(); ++i)
      {
         mStore.mConfigStore.eraseDomain(i->mKey1);
         ++j;
      }
      s << "<p><em>Removed:</em> " << j << " records</p>" << endl;
   }
   
   Dictionary::iterator pos = mHttpParams.find("domainUri");
   if (pos != mHttpParams.end() && (mHttpParams["action"] == "Add")) // found domainUri key
   {
      domainUri = pos->second;
      domainTlsPort = mHttpParams["domainTlsPort"].convertInt();
      if(mStore.mConfigStore.addDomain(domainUri,domainTlsPort))
      {
         s << "<p><em>Added</em> domain: " << domainUri << "</p>" << endl;
      }
      else
      {
         s << "<p><em>Error</em> adding domain: likely database error (check logs).</p>\n";
      }
   }   

   s <<
      "     <h2>Domains</h2>" << endl <<
      "     <form id=\"domainForm\" method=\"get\" action=\"domains.html\" name=\"domainForm\">" << endl <<
      "        <table" REPRO_BORDERLESS_TABLE_PROPS ">" << endl <<
      "          <tr>" << endl <<
      "            <td align=\"right\">New Domain:</td>" << endl <<
      "            <td><input type=\"text\" name=\"domainUri\" size=\"24\"/></td>" << endl <<
      "            <td><input type=\"text\" name=\"domainTlsPort\" size=\"4\"/></td>" << endl <<
      "            <td><input type=\"submit\" name=\"action\" value=\"Add\"/></td>" << endl <<
      "          </tr>" << endl <<
      "        </table>" << endl <<
      "      <div class=space>" << endl <<
      "        <br>" << endl <<
      "      </div>" << endl <<
      "      <table" REPRO_BORDERED_TABLE_PROPS ">" << endl <<
      "        <thead>" << endl <<
      "          <tr>" << endl <<
      "            <td>Domain</td>" << endl <<
      "            <td align=\"center\">TLS Port</td>" << endl <<
      "            <td><input type=\"submit\" name=\"action\" value=\"Remove\"/></td>" << endl << 
      "          </tr>" << endl <<
      "        </thead>" << endl <<
      "        <tbody>" << endl;
   
   const ConfigStore::ConfigData& configs = mStore.mConfigStore.getConfigs();
   for ( ConfigStore::ConfigData::const_iterator i = configs.begin();
        i != configs.end(); i++ )
   {
      s << 
         "          <tr>" << endl <<
         "            <td>" << i->second.mDomain << "</td>" << endl <<
         "            <td align=\"center\">" << i->second.mTlsPort << "</td>" << endl <<
         "            <td><input type=\"checkbox\" name=\"remove." << i->second.mDomain << "\"/></td>" << endl <<
         "          </tr>" << endl;
   }
   
   s <<
      "        </tbody>" << endl <<
      "      </table>" << endl <<
      "     </form>" << endl <<
      "<p><em>WARNING:</em>  You must restart repro after adding domains.</p>" << endl;
}


void
WebAdmin::buildAclsSubPage(DataStream& s)
{ 
   if (!mRemoveSet.empty() && (mHttpParams["action"] == "Remove"))
   {
      int j = 0;
      for (set<RemoveKey>::iterator i = mRemoveSet.begin(); i != mRemoveSet.end(); ++i)
      {
         mStore.mAclStore.eraseAcl(i->mKey1);
         ++j;
      }
      s << "<p><em>Removed:</em> " << j << " records</p>" << endl;
   }
   
   Dictionary::iterator pos = mHttpParams.find("aclUri");
   if (pos != mHttpParams.end() && (mHttpParams["action"] == "Add")) // found 
   {
      Data hostOrIp = mHttpParams["aclUri"];
      int port = mHttpParams["aclPort"].convertInt();
      TransportType transport = Tuple::toTransport(mHttpParams["aclTransport"]);
      
      if (mStore.mAclStore.addAcl(hostOrIp, port, transport))
      {
         s << "<p><em>Added</em> trusted access for: " << hostOrIp << "</p>\n";
      }
      else 
      {
         s << "<p>Error parsing: " << hostOrIp << "</p>\n";
      }
   }   
   
   s << 
      "     <h2>ACLs</h2>" << endl <<
      "      <form id=\"aclsForm\" method=\"get\" action=\"acls.html\" name=\"aclsForm\">" << endl <<
      "      <div class=space>" << endl <<
      "      </div>" << endl <<
      "        <table" REPRO_BORDERLESS_TABLE_PROPS ">" << endl <<
      "          <tr>" << endl <<
      "            <td align=\"right\">Host or IP:</td>" << endl <<
      "            <td><input type=\"text\" name=\"aclUri\" size=\"24\"/></td>" << endl <<
      "            <td><input type=\"text\" name=\"aclPort\" value=\"0\" size=\"5\"/></td>" << endl <<
      "            <td><select name=\"aclTransport\">" << endl <<
      "                <option selected=\"selected\">UDP</option>" << endl <<
      "                <option>TCP</option>" << endl <<
#ifdef USE_SSL
      "                <option>TLS</option>" << endl <<
#endif
#ifdef USE_DTLS
      "                <option>DTLS</option>" << endl <<
#endif
      "            </select></td>" << endl <<
      "            <td><input type=\"submit\" name=\"action\" value=\"Add\"/></td>" << endl <<
      "          </tr>" << endl <<
      "        </table>" << endl <<
      "      <br>" << endl <<
      "      <table" REPRO_BORDERED_TABLE_PROPS ">" << endl <<
      "        <thead>" << endl <<
      "          <tr>" << endl <<
      "            <td>Host Address or Peer Name</td>" << endl <<
      "            <td>Port</td>" << endl <<
      "            <td>Transport</td>" << endl <<
      "            <td><input type=\"submit\" name=\"action\" value=\"Remove\"/></td>" << endl <<
      "          </tr>" << endl <<
      "        </thead>" << endl <<
      "        <tbody>" << endl;
   
   AclStore::Key key = mStore.mAclStore.getFirstTlsPeerNameKey();
   while (key != Data::Empty)
   {
      s << 
         "          <tr>" << endl << 
         "            <td colspan=\"2\">" << mStore.mAclStore.getTlsPeerName(key) << "</td>" << endl <<
         "            <td>TLS auth</td>" << endl <<
         "            <td><input type=\"checkbox\" name=\"remove." << key << "\"/></td>" << endl <<
         "</tr>" << endl;
         
      key = mStore.mAclStore.getNextTlsPeerNameKey(key);
   }
   key = mStore.mAclStore.getFirstAddressKey();
   while (key != Data::Empty)
   {
      s <<
         "          <tr>" << endl << 
         "            <td>" << mStore.mAclStore.getAddressTuple(key).presentationFormat() << "/"
                            << mStore.mAclStore.getAddressMask(key) << "</td>" << endl <<
         "            <td>" << mStore.mAclStore.getAddressTuple(key).getPort() << "</td>" << endl <<
         "            <td>" << Tuple::toData(mStore.mAclStore.getAddressTuple(key).getType()) << "</td>" << endl <<
         "            <td><input type=\"checkbox\" name=\"remove." << key << "\"/></td>" << endl <<
         "          </tr>" << endl;
      key = mStore.mAclStore.getNextAddressKey(key);
   }
   
   s <<  
      "        </tbody>" << endl <<
      "      </table>" << endl <<
      "     </form>" << endl <<
      
      "<pre>" << endl <<
      "      Input can be in any of these formats" << endl <<
      "      localhost         localhost  (becomes 127.0.0.1/8, ::1/128 and fe80::1/64)" << endl <<
      "      bare hostname     server1" << endl <<
      "      FQDN              server1.example.com" << endl <<
      "      IPv4 address      192.168.1.100" << endl <<
      "      IPv4 + mask       192.168.1.0/24" << endl <<
      "      IPv6 address      ::341:0:23:4bb:0011:2435:abcd" << endl <<
      "      IPv6 + mask       ::341:0:23:4bb:0011:2435:abcd/80" << endl <<
      "      IPv6 reference    [::341:0:23:4bb:0011:2435:abcd]" << endl <<
      "      IPv6 ref + mask   [::341:0:23:4bb:0011:2435:abcd]/64" << endl <<
      "</pre>" << endl <<
      
      "<p>Access lists are used as a whitelist to allow " << endl <<
      "gateways and other trusted nodes to skip authentication.</p>" << endl <<
      "<p>Note:  If hostnames or FQDN's are used then a TLS transport type is" << endl <<
      "assumed.  All other transport types must specify ACLs by address.</p>" << endl;
}


void
WebAdmin::buildAddUserSubPage(DataStream& s)
{
   Dictionary::iterator pos;
   Data user;
   
   pos = mHttpParams.find("user");
   if (pos != mHttpParams.end()) // found user key
   {
      user = pos->second;
      Data domain = mHttpParams["domain"];
      
//      pos = mHttpParams.find("realm");
//      if (pos == mHttpParams.end())
//      {
//         realm = mHttpParams["domain"];
//      }
            
      if(mStore.mUserStore.addUser(user,domain,domain,mHttpParams["password"],true,mHttpParams["name"],mHttpParams["email"]))
      {
         s << "<p><em>Added:</em> " << user << "@" << domain << "</p>\n";
      }
      else
      {
         s << "<p><em>Error</em> adding user: likely database error (check logs).</p>\n";
      }
   }

      s << 
         "<h2>Add User</h2>" << endl <<
         "<form id=\"addUserForm\" action=\"addUser.html\"  method=\"get\" name=\"addUserForm\" enctype=\"application/x-www-form-urlencoded\">" << endl << 
         "<table" REPRO_BORDERLESS_TABLE_PROPS ">" << endl << 
         "<tr>" << endl << 
         "  <td align=\"right\" valign=\"middle\">User Name:</td>" << endl << 
         "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"user\" size=\"40\"/></td>" << endl << 
         "</tr>" << endl << 

         //"<tr>" << endl << 
         //"<td align=\"right\" valign=\"middle\" >Realm:</td>" << endl << 
         //"<td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"realm\" size=\"40\"/></td>" << endl << 
         //"</tr>" << endl << 

         "<tr>" << endl << 
         "  <td align=\"right\" valign=\"middle\" >Domain:</td>" << endl << 
         "  <td align=\"left\" valign=\"middle\"><select name=\"domain\">" << endl
         ; 
         
         // for each domain, add an option in the pulldown         
         const ConfigStore::ConfigData& list = mStore.mConfigStore.getConfigs();
         for ( ConfigStore::ConfigData::const_iterator i = list.begin();
              i != list.end(); i++ )
         {
            s << "            <option";
            
            // if i->Domain is the default domain
            // {
            //    s << " selected=\"true\""; 
            // }
            
            s << ">" << i->second.mDomain << "</option>" << endl;
         }

         s <<
         "</select></td></tr>" << endl <<
         "<tr>" << endl << 
         "  <td align=\"right\" valign=\"middle\" >Password:</td>" << endl << 
         "  <td align=\"left\" valign=\"middle\"><input type=\"password\" name=\"password\" size=\"40\"/></td>" << endl << 
         "</tr>" << endl << 

         "<tr>" << endl << 
         "  <td align=\"right\" valign=\"middle\" >Full Name:</td>" << endl << 
         "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"name\" size=\"40\"/></td>" << endl << 
         "</tr>" << endl << 

         "<tr>" << endl << 
         "  <td align=\"right\" valign=\"middle\" >Email:</td>" << endl << 
         "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"email\" size=\"40\"/></td>" << endl << 
         "</tr>" << endl << 

         "<tr>" << endl << 
         "  <td colspan=\"2\" align=\"right\" valign=\"middle\">" << endl << 
         "    <input type=\"reset\" value=\"Cancel\"/>" << endl << 
         "    <input type=\"submit\" name=\"submit\" value=\"Add\"/>" << endl << 
         "  </td>" << endl << 
         "</tr>" << endl << 
         
         "</table>" << endl << 
         "</form>" << endl
         ;
}


void
WebAdmin::buildEditUserSubPage(DataStream& s)
{
   Dictionary::iterator pos;
   pos = mHttpParams.find("key");
   if (pos != mHttpParams.end()) 
   {
      Data key = pos->second;
      AbstractDb::UserRecord rec = mStore.mUserStore.getUserInfo(key);
      // !rwm! TODO check to see if we actually found a record corresponding to the key.  how do we do that?
      
      s << "<h2>Edit User</h2>" << endl <<
           "<p>Editing Record with key: " << key << "</p>" << endl <<
           "<p>Note:  If the username is not modified and you leave the password field empty the users current password will not be reset.</p>" << endl;
      
      s << 
         "<form id=\"editUserForm\" action=\"showUsers.html\"  method=\"get\" name=\"editUserForm\" enctype=\"application/x-www-form-urlencoded\">" << endl << 
         "<table" REPRO_BORDERLESS_TABLE_PROPS ">" << endl << 
         "<input type=\"hidden\" name=\"key\" value=\"" << key << "\"/>" << endl << 
         "<tr>" << endl << 
         "  <td align=\"right\" valign=\"middle\">User Name:</td>" << endl << 
         "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"user\" value=\"" << rec.user << "\" size=\"40\"/></td>" << endl << 
         "</tr>" << endl << 
         
         //"<tr>" << endl << 
         //"<td align=\"right\" valign=\"middle\" >Realm:</td>" << endl << 
         //"<td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"realm\" size=\"40\"/></td>" << endl << 
         //"</tr>" << endl << 

         "<tr>" << endl << 
         "  <td align=\"right\" valign=\"middle\" >Domain:</td>" << endl << 
         "  <td align=\"left\" valign=\"middle\"><select name=\"domain\">" << endl
         ; 
      
      // for each domain, add an option in the pulldown      
      const ConfigStore::ConfigData& list = mStore.mConfigStore.getConfigs();      
      for ( ConfigStore::ConfigData::const_iterator i = list.begin();
            i != list.end(); i++ )
      {
         s << "            <option";
         
         if ( i->second.mDomain == rec.domain)
         {
            s << " selected=\"true\""; 
         }
         
         s << ">" << i->second.mDomain << "</option>" << endl;
      }
      
      s <<
         "</select></td></tr>" << endl <<
         "<tr>" << endl << 
         "  <td align=\"right\" valign=\"middle\" >Password:</td>" << endl << 
         "  <td align=\"left\" valign=\"middle\"><input type=\"password\" name=\"password\" size=\"40\"/></td>" << endl << 
         "</tr>" << endl << 
         // Note that the UserStore only stores a passwordHash, so we will collect a password.  If one is provided in the
         // edit page, we will use it to generate a new passwordHash, otherwise we will leave the hash alone.
         
         "<tr>" << endl << 
         "  <td align=\"right\" valign=\"middle\" >Full Name:</td>" << endl << 
         "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"name\" value=\"" << rec.name << 
         "\" size=\"40\"/></td>" << endl << 
         "</tr>" << endl << 

         "<tr>" << endl << 
         "  <td align=\"right\" valign=\"middle\" >Email:</td>" << endl << 
         "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"email\" value=\"" << rec.email <<
         "\" size=\"40\"/></td>" << endl << 
         "</tr>" << endl << 

         "<tr>" << endl << 
         "  <td colspan=\"2\" align=\"right\" valign=\"middle\">" << endl << 
         "    <input type=\"submit\" name=\"submit\" value=\"Update\"/>" << endl << 
         "  </td>" << endl << 
         "</tr>" << endl << 
         
         "</table>" << endl <<
         "</form>" << endl
         ;
   }
   else
   {
      // go back to show users page
   }
}


void 
WebAdmin::buildShowUsersSubPage(DataStream& s)
{
   Dictionary::iterator pos;
   Data key;
   AbstractDb::UserRecord rec;

   if (!mRemoveSet.empty())
   {
      int j = 0;
      for (set<RemoveKey>::iterator i = mRemoveSet.begin(); i != mRemoveSet.end(); ++i)
      {
         mStore.mUserStore.eraseUser(i->mKey1);
         ++j;
      }
      s << "<p><em>Removed:</em> " << j << " records</p>" << endl;
   }
   
   pos = mHttpParams.find("key");
   if (pos != mHttpParams.end())  // check if the user parameter exists, if so, use as a key to update the record
   {
      key = pos->second;
      rec = mStore.mUserStore.getUserInfo(key);
      // check to see if we actually found a record corresponding to the key
      if (!rec.user.empty())
      {
         Data user = mHttpParams["user"];
         Data domain = mHttpParams["domain"];                  
         Data realm = mHttpParams["domain"];   // eventually sort out realms
         Data password = mHttpParams["password"];
         Data passwordHashAlt = Data::Empty;
         Data name = mHttpParams["name"];
         Data email = mHttpParams["email"];
         bool applyA1HashToPassword = true;
         
         // if no password was specified, then leave current password untouched
         if(password == "" && user == rec.user && realm == rec.realm) 
         {
            password = rec.passwordHash;
            passwordHashAlt = rec.passwordHashAlt;
            applyA1HashToPassword = false;
         }
         // write out the updated record to the database now
         if(mStore.mUserStore.updateUser(key, user, domain, realm, password, applyA1HashToPassword, name, email, passwordHashAlt))
         {
            s << "<p><em>Updated:</em> " << key << "</p>" << endl; 
         }
         else
         {
            s << "<p><em>Error</em> updating user: likely database error (check logs).</p>\n";
         }
      }
   }
   
      
   s << 
      "<h2>Users</h2>" << endl <<
      "<form id=\"showUsers\" method=\"get\" action=\"showUsers.html\" name=\"showUsers\" enctype=\"application/x-www-form-urlencoded\">" << endl << 
      "<table" REPRO_BORDERED_TABLE_PROPS ">" << endl << 
      "<tr>" << endl << 
      "  <td>User@Domain</td>" << endl << 
      //  "  <td>Realm</td>" << endl << 
      "  <td>Name</td>" << endl << 
      "  <td>Email</td>" << endl << 
      "  <td><input type=\"submit\" value=\"Remove\"/></td>" << endl << 
      "</tr>" << endl;
   
   s << endl;
   
   int count =0;
   
   key = mStore.mUserStore.getFirstKey();
   while ( !key.empty() )
   {
      rec = mStore.mUserStore.getUserInfo(key);

      if ((rec.domain == Data::Empty) && (rec.user == "admin"))
      {
         key = mStore.mUserStore.getNextKey();
         continue;   // skip the row for the admin web user
      }
      
      s << "<tr>" << endl 
        << "  <td><a href=\"editUser.html?key=";
      key.urlEncode(s);
      s << "\">" << rec.user << "@" << rec.domain << "</a></td>" << endl
        << "  <td>" << rec.name << "</td>" << endl
        << "  <td>" << rec.email << "</td>" << endl
        << "  <td><input type=\"checkbox\" name=\"remove." << key << "\"/></td>" << endl
        << "</tr>" << endl;
         
      key = mStore.mUserStore.getNextKey();

      // make a limit to how many users are displayed 
      if ( ++count > 1000 )
      {
         break;
      }
   }
   
   if ( !key.empty() )
   {
      s << "<tr><td>Only first 1000 users were displayed<td></tr>" << endl;
   }
   
   s << 
      "</table>" << endl << 
      "</form>" << endl;
}

void
WebAdmin::buildAddFilterSubPage(DataStream& s)
{
   Dictionary::iterator pos;

   pos = mHttpParams.find("cond1header");
   if (pos != mHttpParams.end())
   {
      Data action = mHttpParams["action"];
      Data actionData = mHttpParams["actiondata"];
      
      if (action != "Accept" && actionData.empty())
      {
         s << "<p><em>Error</em> adding request filter.  You must provide appropriate Action Data for non-Accept action.</p>\n";
      }
      else
      {
         short actionShort = 0;  // 0 - Accept, 1 - Reject, 2 - SQL Query
         if(action == "Reject") actionShort = 1;
         else if(action == "SQL Query") actionShort = 2;

         if(mStore.mFilterStore.addFilter(mHttpParams["cond1header"],
                                          mHttpParams["cond1regex"],
                                          mHttpParams["cond2header"],
                                          mHttpParams["cond2regex"],
                                          mHttpParams["method"], 
                                          mHttpParams["event"],
                                          actionShort,
                                          actionData,
                                          mHttpParams["order"].convertInt()))
         {
            s << "<p><em>Added</em> request filter: " << mHttpParams["cond1header"] << "=" << mHttpParams["cond1regex"] << ", "
                                                      << mHttpParams["cond2header"] << "=" << mHttpParams["cond2regex"] << "</p>\n";
         }
         else
         {
            s << "<p><em>Error</em> adding request filter, likely duplicate found.</p>\n";
         }
      }
   }

   s << 
      "<h2>Add Request Filter</h2>" << endl <<
      "<form id=\"addFilterForm\" method=\"get\" action=\"addFilter.html\" name=\"addFilterForm\">" << endl << 
      "<table" REPRO_BORDERLESS_TABLE_PROPS ">" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Condition1 Header:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"cond1header\" size=\"40\" value=\"From\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Condition1 Regex:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"cond1regex\" size=\"40\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Condition2 Header:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"cond2header\" size=\"40\" value=\"To\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Condition2 Regex:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"cond2regex\" size=\"40\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Method:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"method\" size=\"40\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Event:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"event\" size=\"40\"/></td>" << endl << 
      "</tr>" << endl << 
      
      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Action:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\">" << endl <<
      "    <select name=\"action\">" << endl <<
      "      <option>Reject</option>" << endl << 
      "      <option>Accept</option>" << endl << 
#ifdef USE_MYSQL
      "      <option>SQL Query</option>" << endl << 
#endif
      "    </select>" << endl <<
      "  </td>" << endl <<
      "</tr>" << endl <<

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Action Data:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"actiondata\" size=\"40\" value=\"403, Request Blocked\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Order:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"order\" size=\"4\" value=\"0\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td colspan=\"2\" align=\"right\" valign=\"middle\">" << endl << 
      "    <input type=\"reset\"  value=\"Cancel\"/>" << endl << 
      "    <input type=\"submit\" name=\"filterAdd\" value=\"Add\"/>" << endl << 
      "  </td>" << endl << 
      "</tr>" << endl << 

      "</table>" << endl << 
      "</form>" << endl <<

      "<pre>" << endl <<
      "If Action is Accept, then Action Data is ignored." << endl <<
      "If Action is Reject, then Action Data should be set to: SIPRejectionCode[, SIPReason]"  << endl <<
#ifdef USE_MYSQL
      "If Action is SQL Query, then Action Data should be set to the SQL Query to execute." << endl <<
      "Replacement strings from the Regex's above can be used in the query, and the query" << endl <<
      "must return a string that is formated similar to Action Data when the action is" << endl <<
      "Reject.  Alternatively it can return a string with status code of 0 to accept the" << endl <<
      "request." << endl <<
#endif
      "</pre>" << endl;
}


void
WebAdmin::buildEditFilterSubPage(DataStream& s)
{
   Dictionary::iterator pos;
   pos = mHttpParams.find("key");
   if (pos != mHttpParams.end()) 
   {
      Data key = pos->second;

      // !rwm! TODO check to see if we actually found a record corresponding to the key.  how do we do that?
      DebugLog( << "Creating page to edit filter " << key );
      
      AbstractDb::FilterRecord rec = mStore.mFilterStore.getFilterRecord(key);

      s <<"<h2>Edit Request Filter</h2>" << endl <<
         "<p>Editing Record with conditions: " << rec.mCondition1Header << "=" << rec.mCondition1Regex << ", "
                                               << rec.mCondition2Header << "=" << rec.mCondition2Regex << "</p>" << endl;

      s << 
      "<form id=\"editFilterForm\" method=\"get\" action=\"showFilters.html\" name=\"editFilterForm\">" << endl << 
      "<table" REPRO_BORDERLESS_TABLE_PROPS ">" << endl << 
      "<input type=\"hidden\" name=\"key\" value=\"" << key << "\"/>" << endl << 
      "<tr>" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Condition1 Header:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"cond1header\" size=\"40\" value=\"" << rec.mCondition1Header.xmlCharDataEncode() << "\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Condition1 Regex:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"cond1regex\" size=\"40\" value=\"" << rec.mCondition1Regex.xmlCharDataEncode() << "\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Condition2 Header:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"cond2header\" size=\"40\" value=\"" << rec.mCondition2Header.xmlCharDataEncode() << "\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Condition2 Regex:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"cond2regex\" size=\"40\" value=\"" << rec.mCondition2Regex.xmlCharDataEncode() << "\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Method:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"method\" size=\"40\" value=\"" << rec.mMethod << "\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Event:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"event\" size=\"40\" value=\"" << rec.mEvent << "\"/></td>" << endl << 
      "</tr>" << endl << 
      
      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Action:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\">" << endl <<
      "    <select name=\"action\">" << endl <<
      "      <option" << (rec.mAction == FilterStore::Reject ? " selected=\"selected\"" : "") << ">Reject</option>" << endl << 
      "      <option" << (rec.mAction == FilterStore::Accept ? " selected=\"selected\"" : "") << ">Accept</option>" << endl << 
#ifdef USE_MYSQL
      "      <option" << (rec.mAction == FilterStore::SQLQuery ? " selected=\"selected\"" : "") << ">SQL Query</option>" << endl << 
#endif
      "    </select>" << endl <<
      "  </td>" << endl <<
      "</tr>" << endl <<

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Action Data:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"actiondata\" size=\"40\" value=\"" << rec.mActionData.xmlCharDataEncode() << "\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Order:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"order\" size=\"4\" value=\"" << rec.mOrder << "\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td colspan=\"2\" align=\"right\" valign=\"middle\">" << endl << 
      "    <input type=\"submit\" name=\"routeEdit\" value=\"Update\"/>" << endl << 
      "  </td>" << endl << 
      "</tr>" << endl << 

      "</table>" << endl << 
      "</form>" << endl;
   }
   else
   {
      // go back to show filter page
   }
}


void
WebAdmin::buildShowFiltersSubPage(DataStream& s)
{
   Dictionary::iterator pos;
   Data key;
   AbstractDb::RouteRecord rec;

   if (!mRemoveSet.empty())
   {
      int j = 0;
      for (set<RemoveKey>::iterator i = mRemoveSet.begin(); i != mRemoveSet.end(); ++i)
      {
         mStore.mFilterStore.eraseFilter(i->mKey1);
         ++j;
      }
      s << "<p><em>Removed:</em> " << j << " records</p>" << endl;
   }
   
   pos = mHttpParams.find("key");
   if (pos != mHttpParams.end())   // if a key parameter exists, use the key to update the record
   {
      key = pos->second;

      // !rwm! TODO check to see if we actually found a record corresponding to the key.  how do we do that?
      if (1)
      {
         Data action = mHttpParams["action"];
         Data actionData = mHttpParams["actiondata"];
      
         if (action != "Accept" && actionData.empty())
         {
            s << "<p><em>Error</em> updating request filter.  You must provide appropriate Action Data for non-Accept action.</p>\n";
         }
         else
         {
            short actionShort = 0;  // 0 - Accept, 1 - Reject, 2 - SQL Query
            if(action == "Reject") actionShort = 1;
            else if(action == "SQL Query") actionShort = 2;

            if(mStore.mFilterStore.updateFilter(key,
                                             mHttpParams["cond1header"],
                                             mHttpParams["cond1regex"],
                                             mHttpParams["cond2header"],
                                             mHttpParams["cond2regex"],
                                             mHttpParams["method"], 
                                             mHttpParams["event"],
                                             actionShort,
                                             actionData,
                                             mHttpParams["order"].convertInt()))
            {
               s << "<p><em>Updated</em> request filter: " << mHttpParams["cond1header"] << "=" << mHttpParams["cond1regex"] << ", "
                                                           << mHttpParams["cond2header"] << "=" << mHttpParams["cond2regex"] << "</p>\n";
            }
            else
            {
               s << "<p><em>Error</em> updating request filter: likely database error (check logs).</p>\n";
            }
         }
      }
   }

   s <<
      "<h2>Request Filters</h2>" << endl <<
      "<form id=\"showFilters\" action=\"showFilters.html\" method=\"get\" name=\"showFilters\" enctype=\"application/x-www-form-urlencoded\">" << endl << 
      // "            <button name=\"removeAllRoute\" value=\"\" type=\"submit\">Remove All</button>" << endl << 
      "<table" REPRO_BORDERED_TABLE_PROPS ">" << endl << 
      "<thead><tr>" << endl << 
      "  <td>Condition 1</td>" << endl << 
      "  <td>Condition 2</td>" << endl << 
      "  <td>Method</td>" << endl << 
      "  <td>Event</td>" << endl << 
      "  <td>Action</td>" << endl << 
      "  <td>Action Data</td>" << endl << 
      "  <td>Order</td>" << endl << 
      "  <td><input type=\"submit\" value=\"Remove\"/></td>" << endl << 
      "</tr></thead>" << endl << 
      "<tbody>" << endl;

   for(FilterStore::Key key = mStore.mFilterStore.getFirstKey();
       !key.empty();
        key = mStore.mFilterStore.getNextKey(key))
   {
      AbstractDb::FilterRecord rec = mStore.mFilterStore.getFilterRecord(key);
      Data action("Accept");
      if(rec.mAction == FilterStore::Reject)
      {
         action = "Reject";
      }
      else if(rec.mAction == FilterStore::SQLQuery)
      {
         action = "SQL Query";
      }
      s <<  "<tr>" << endl << 
         "<td><a href=\"editFilter.html?key=";
            key.urlEncode(s); 
      s << 
         "\">" << rec.mCondition1Header << "=" << rec.mCondition1Regex << "</a></td>" << endl << 
         "<td>" << rec.mCondition2Header << "=" << rec.mCondition2Regex << "</td>" << endl << 
         "<td>" << rec.mMethod << "</td>" << endl << 
         "<td>" << rec.mEvent << "</td>" << endl << 
         "<td>" << action << "</td>" << endl << 
         "<td>" << rec.mActionData << "</td>" << endl << 
         "<td>" << rec.mOrder << "</td>" << endl << 
         "<td><input type=\"checkbox\" name=\"remove." <<  key << "\"/></td>" << endl << 
         "</tr>" << endl;
   }

   s << 
      "</tbody>" << endl << 
      "</table>" << endl << 
      "</form>" << endl;

   Data cond1TestHeader;
   pos = mHttpParams.find("cond1TestHeader");
   if (pos != mHttpParams.end()) // found it
   {
      cond1TestHeader = pos->second;
   }
   Data cond2TestHeader;
   pos = mHttpParams.find("cond2TestHeader");
   if (pos != mHttpParams.end()) // found it
   {
      cond2TestHeader = pos->second;
   }

   s << 
      "<br><form id=\"testFilter\" action=\"showFilters.html\" method=\"get\" name=\"testFilter\" enctype=\"application/x-www-form-urlencoded\">" << endl << 
      "<table" REPRO_BORDERLESS_TABLE_PROPS ">" << endl << 
      "<tr>" << endl << 
      "  <td align=\"right\">Condition 1 Header:</td>" << endl << 
      "  <td><input type=\"text\" name=\"cond1TestHeader\" value=\"" << cond1TestHeader.xmlCharDataEncode() << "\" size=\"40\"/></td>" << endl << 
      "</tr>" << endl <<
      "<tr>" << endl << 
      "  <td align=\"right\">Condition 2 Header:</td>" << endl << 
      "  <td><input type=\"text\" name=\"cond2TestHeader\" value=\"" << cond2TestHeader.xmlCharDataEncode() << "\" size=\"40\"/></td>" << endl << 
      "  <td><input type=\"submit\" name=\"testFilter\" value=\"Test Filters\"/></td>" << endl << 
      "</tr>" << endl <<
      "</table>" << endl << 
      "</form>" << endl <<
      "<br>" << endl;
   
   if(!cond1TestHeader.empty())
   {
      s << "<em>Test Result: </em>";
      short action;
      Data actionData;
      if(mStore.mFilterStore.test(cond1TestHeader, cond2TestHeader, action, actionData))
      {
         switch(action)
         {
         case FilterStore::Reject:
            s << "Match found, action=Reject " << actionData << endl;
            break;
         case FilterStore::SQLQuery:
            s << "Match found, action=SQL Query '" << actionData << "'" << endl;
            break;
         case FilterStore::Accept:
         default:
            s << "Match found, action=Accept" << endl;
            break;
         }
      }
      else
      {
         s << "No Match";
      }
   }
}


void
WebAdmin::buildAddRouteSubPage(DataStream& s)
{
   Dictionary::iterator pos;

   pos = mHttpParams.find("routeUri");
   if (pos != mHttpParams.end())
   {
      Data routeUri = mHttpParams["routeUri"];
      Data routeDestination = mHttpParams["routeDestination"];
      
      if (!routeUri.empty() && !routeDestination.empty())
      {
         if(mStore.mRouteStore.addRoute(mHttpParams["routeMethod"], 
                                        mHttpParams["routeEvent"], 
                                        routeUri,
                                        routeDestination,
                                        mHttpParams["routeOrder"].convertInt()))
         {
            s << "<p><em>Added</em> route for: " << routeUri << "</p>\n";
         }
         else
         {
            s << "<p><em>Error</em> adding route, likely duplicate found.</p>\n";
         }
      }
      else
      {
         s << "<p><em>Error</em> adding route.  You must provide a URI and a route destination.</p>\n";
      }
   }

   s << 
      "<h2>Add Route</h2>" << endl <<
      "<form id=\"addRouteForm\" method=\"get\" action=\"addRoute.html\" name=\"addRouteForm\">" << endl << 
      "<table" REPRO_BORDERLESS_TABLE_PROPS ">" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">URI:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"routeUri\" size=\"40\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Method:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"routeMethod\" size=\"40\"/></td>" << endl << 
      "</tr>" << endl << 
      
      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Event:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"routeEvent\" size=\"40\"/></td>" << endl << 
      "</tr>" << endl << 
      
      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Destination:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"routeDestination\" size=\"40\"/></td>" << endl << 
      "</tr>" << endl << 
      
      "<tr>" << endl << 
      "  <td align=\"right\" valign=\"middle\">Order:</td>" << endl << 
      "  <td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"routeOrder\" size=\"4\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td colspan=\"2\" align=\"right\" valign=\"middle\">" << endl << 
      "    <input type=\"reset\"  value=\"Cancel\"/>" << endl << 
      "    <input type=\"submit\" name=\"routeAdd\" value=\"Add\"/>" << endl << 
      "  </td>" << endl << 
      "</tr>" << endl << 

      "</table>" << endl << 
      "</form>" << endl <<

      "<pre>" << endl <<
      "Static routes use (POSIX-standard) regular expression to match" << endl <<
      "and rewrite SIP URIs.  The following is an example of sending" << endl <<
      "all requests that consist of only digits in the userpart of the" << endl <<
      "SIP URI to a gateway:" << endl << endl <<
      "   URI:         ^sip:([0-9]+)@example\\.com" << endl <<
      "   Destination: sip:$1@gateway.example.com" << endl <<
      "</pre>" << endl;
}


void
WebAdmin::buildEditRouteSubPage(DataStream& s)
{
   Dictionary::iterator pos;
   pos = mHttpParams.find("key");
   if (pos != mHttpParams.end()) 
   {
      Data key = pos->second;

      // !rwm! TODO check to see if we actually found a record corresponding to the key.  how do we do that?
      DebugLog( << "Creating page to edit route " << key );
      
      AbstractDb::RouteRecord rec = mStore.mRouteStore.getRouteRecord(key);

      s <<"<h2>Edit Route</h2>" << endl <<
          "<p>Editing Record with matching pattern: " << rec.mMatchingPattern << "</p>" << endl;      

      s << 
      "<form id=\"editRouteForm\" method=\"get\" action=\"showRoutes.html\" name=\"editRouteForm\">" << endl << 
      "<table" REPRO_BORDERLESS_TABLE_PROPS ">" << endl << 
      "<input type=\"hidden\" name=\"key\" value=\"" << key << "\"/>" << endl << 
      "<tr>" << endl << 
      "<td align=\"right\" valign=\"middle\">URI:</td>" << endl << 
      "<td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"routeUri\" value=\"" <<  rec.mMatchingPattern << "\" size=\"40\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "<td align=\"right\" valign=\"middle\">Method:</td>" << endl << 
      "<td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"routeMethod\" value=\"" <<  rec.mMethod  << "\" size=\"40\"/></td>" << endl << 
      "</tr>" << endl << 
      
      "<tr>" << endl << 
      "<td align=\"right\" valign=\"middle\">Event:</td>" << endl << 
      "<td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"routeEvent\" value=\"" << rec.mEvent  << "\" size=\"40\"/></td>" << endl << 
      "</tr>" << endl << 
      
      "<tr>" << endl << 
      "<td align=\"right\" valign=\"middle\">Destination:</td>" << endl << 
      "<td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"routeDestination\" value=\"" << rec.mRewriteExpression <<
                            "\" size=\"40\"/></td>" << endl << 
      "</tr>" << endl << 
      
      "<tr>" << endl << 
      "<td align=\"right\" valign=\"middle\">Order:</td>" << endl << 
      "<td align=\"left\" valign=\"middle\"><input type=\"text\" name=\"routeOrder\" value=\"" << rec.mOrder  <<
                            "\" size=\"4\"/></td>" << endl << 
      "</tr>" << endl << 

      "<tr>" << endl << 
      "  <td colspan=\"2\" align=\"right\" valign=\"middle\">" << endl << 
      "    <input type=\"submit\" name=\"routeEdit\" value=\"Update\"/>" << endl << 
      "  </td>" << endl << 
      "</tr>" << endl << 

      "</table>" << endl << 
      "</form>" << endl;
   }
   else
   {
      // go back to show route page
   }
}


void
WebAdmin::buildShowRoutesSubPage(DataStream& s)
{
   Dictionary::iterator pos;
   Data key;
   AbstractDb::RouteRecord rec;

   if (!mRemoveSet.empty())
   {
      int j = 0;
      for (set<RemoveKey>::iterator i = mRemoveSet.begin(); i != mRemoveSet.end(); ++i)
      {
         mStore.mRouteStore.eraseRoute(i->mKey1);
         ++j;
      }
      s << "<p><em>Removed:</em> " << j << " records</p>" << endl;
   }
   
   pos = mHttpParams.find("key");
   if (pos != mHttpParams.end())   // if a key parameter exists, use the key to update the record
   {
      key = pos->second;

      // !rwm! TODO check to see if we actually found a record corresponding to the key.  how do we do that?
      if (1)
      {
         Data method = mHttpParams["routeMethod"]; 
         Data event = mHttpParams["routeEvent"]; 
         Data matchingPattern = mHttpParams["routeUri"];
         Data rewriteExpression = mHttpParams["routeDestination"];
         int  order = mHttpParams["routeOrder"].convertInt();
         
         if (!matchingPattern.empty() && !rewriteExpression.empty())
         {
            // write out the updated record to the database now
            if(mStore.mRouteStore.updateRoute(key, method, event, matchingPattern, rewriteExpression, order))
            {
               s << "<p><em>Updated:</em> " << rec.mMatchingPattern << "</p>" << endl; 
            }
            else
            {
               s << "<p><em>Error</em> updating route: likely database error (check logs).</p>\n";
            }
         }
         else
         {
            s << "<p><em>Error</em> updating route.  You must provide a URI and a route destination.</p>\n";
         }
      }
   }

   s <<
      "<h2>Routes</h2>" << endl <<
      "<form id=\"showRoutes\" action=\"showRoutes.html\" method=\"get\" name=\"showRoutes\" enctype=\"application/x-www-form-urlencoded\">" << endl << 
      // "            <button name=\"removeAllRoute\" value=\"\" type=\"submit\">Remove All</button>" << endl << 
      "<table" REPRO_BORDERED_TABLE_PROPS ">" << endl << 
      "<thead><tr>" << endl << 
      "  <td>URI</td>" << endl << 
      "  <td>Method</td>" << endl << 
      "  <td>Event</td>" << endl << 
      "  <td>Destination</td>" << endl << 
      "  <td>Order</td>" << endl << 
      "  <td><input type=\"submit\" value=\"Remove\"/></td>" << endl << 
      "</tr></thead>" << endl << 
      "<tbody>" << endl;
   
   for ( RouteStore::Key key = mStore.mRouteStore.getFirstKey();
         !key.empty();
         key = mStore.mRouteStore.getNextKey(key) )
   {
      AbstractDb::RouteRecord rec = mStore.mRouteStore.getRouteRecord(key);

      s <<  "<tr>" << endl << 
         "<td><a href=\"editRoute.html?key=";
            key.urlEncode(s); 
      s << 
         "\">" << rec.mMatchingPattern << "</a></td>" << endl << 
         "<td>" << rec.mMethod << "</td>" << endl << 
         "<td>" << rec.mEvent << "</td>" << endl << 
         "<td>" << rec.mRewriteExpression << "</td>" << endl << 
         "<td>" << rec.mOrder << "</td>" << endl << 
         "<td><input type=\"checkbox\" name=\"remove." <<  key << "\"/></td>" << endl << 
         "</tr>" << endl;
   }
   
   s << 
      "</tbody>" << endl << 
      "</table>" << endl << 
      "</form>" << endl;

   int badUri = true;
   Uri uri;
   Data routeTestUri;
   
   pos = mHttpParams.find("routeTestUri");
   if (pos != mHttpParams.end()) // found it
   {
      routeTestUri = pos->second;
      if ( routeTestUri  != "sip:" )
      {
         try 
         {
            uri = Uri(routeTestUri);
            badUri=false;
         }
         catch( BaseException&  )
         {
            try 
            {
               uri = Uri( Data("sip:")+routeTestUri );
               badUri=false;
            }
            catch( BaseException&  )
            {
            }
         }
      }
   }
      
   // !cj! - TODO - should input method and event type to test 
   RouteStore::UriList routeList;
   if (!badUri)
   {
      routeList = mStore.mRouteStore.process(uri, Data("INVITE"), Data::Empty);
   }
   
   s << 
      "<br><form id=\"testRoute\" action=\"showRoutes.html\" method=\"get\" name=\"testRoute\" enctype=\"application/x-www-form-urlencoded\">" << endl << 
      "<table" REPRO_BORDERLESS_TABLE_PROPS ">" << endl << 
      "<tr>" << endl << 
      " <td align=\"right\">Input:</td>" << endl << 
      " <td><input type=\"text\" name=\"routeTestUri\" value=\"" << uri << "\" size=\"40\"/></td>" << endl << 
      " <td><input type=\"submit\" name=\"testRoute\" value=\"Test Routes\"/></td>" << endl << 
      "</tr>" << endl;
   
   bool first=true;
   for ( RouteStore::UriList::const_iterator i=routeList.begin();
         i != routeList.end(); i++)
   {
      s<<"              <tr>" << endl;
      if (first)
      {
         first=false;
         s<<"             <td align=\"right\">Targets:</td>" << endl;
      }
      else
      {
         s<<"             <td align=\"right\"></td>" << endl;
      }
      s<<"                <td><label>" << *i << "</label></td>" << endl;
      s<<"                <td></td>" << endl;
      s<<"              </tr>" << endl;
   }
   
   s<<
      "</table>" << endl << 
      "</form>" << endl;
}


void
WebAdmin::buildRegistrationsSubPage(DataStream& s)
{
   if (!mRemoveSet.empty())
   {
      int j = 0;
      for (set<RemoveKey>::iterator i = mRemoveSet.begin(); i != mRemoveSet.end(); ++i)
      {
         Uri aor(i->mKey1);
         ContactInstanceRecord rec;
         size_t bar1 = i->mKey2.find("|");
         size_t bar2 = i->mKey2.find("|",bar1+1);
         size_t bar3 = i->mKey2.find("|",bar2+1);
         
         if(bar1==Data::npos || bar2 == Data::npos || bar3==Data::npos)
         {
            InfoLog(<< "Registration removal key was malformed: " << i->mKey2);
            continue;
         }
         
         bool staticRegContact=false;
         try
         {
            resip::Data rawNameAddr = i->mKey2.substr(0,bar1).urlDecoded();
            rec.mContact = NameAddr(rawNameAddr);
            rec.mInstance = i->mKey2.substr(bar1+1,bar2-bar1-1).urlDecoded();
            rec.mRegId = i->mKey2.substr(bar2+1,Data::npos).convertInt();
            staticRegContact = i->mKey2.substr(bar3+1,Data::npos).convertInt() == 1;

            // Remove from RegistrationPersistanceManager
            mRegDb.removeContact(aor, rec);

            if(staticRegContact)
            {
               // Remove from StateRegStore
               mStore.mStaticRegStore.eraseStaticReg(aor, rec.mContact);
            }

            ++j;
         }
         catch(resip::ParseBuffer::Exception& e)
         {
            InfoLog(<< "Registration removal key was malformed: " << e <<
                     " Key was: " << i->mKey2);
         }
      }
      s << "<p><em>Removed:</em> " << j << " records</p>" << endl;
   }

   Dictionary::iterator pos = mHttpParams.find("regAor");
   if (pos != mHttpParams.end() && (mHttpParams["action"] == "Add")) // found 
   {
      Data regAor = mHttpParams["regAor"];
      Data regContact = mHttpParams["regContact"];
      Data regPath = mHttpParams["regPath"];

      ContactInstanceRecord rec;
      try
      {
         rec.mContact = NameAddr(regContact);
         try
         {
            ParseBuffer pb(regPath);
            Data path;
            const char* anchor = pb.position();
            while(!pb.eof())
            {
               pb.skipToChar(Symbols::COMMA[0]);
               pb.data(path, anchor);
               rec.mSipPath.push_back(NameAddr(path));
               if(!pb.eof()) pb.skipChar();  // skip over comma
               anchor = pb.position();
            }
            try
            {
               rec.mRegExpires = NeverExpire;
               rec.mSyncContact = true;  // Tag this permanent contact as being a syncronized contact so that it will
                                      // be syncronized to a paired server (this is actually configuration information)

               // Add to DB Store
               Uri aor(regAor);
               if(mStore.mStaticRegStore.addStaticReg(aor, rec.mContact, rec.mSipPath))
               {   
                  // Add to RegistrationPersistanceManager
                  mRegDb.updateContact(aor, rec);

                  s << "<p><em>Added</em> permanent registered contact for: " << regAor << "</p>\n";
               }
               else
               {
                  s << "<p><em>Error</em> adding static registration: likely database error (check logs).</p>\n";
               }
            }
            catch(resip::ParseBuffer::Exception& e)
            {
               InfoLog(<< "Registration add: aor " << regAor << " was malformed: " << e);
               s << "<p>Error parsing: AOR=" << regAor << "</p>\n";
            }  
         }
         catch(resip::ParseBuffer::Exception& e)
         {
            InfoLog(<< "Registration add: path " << regPath << " was malformed: " << e);
            s << "<p>Error parsing: Path=" << regPath << "</p>\n";
         }
      }
      catch(resip::ParseBuffer::Exception& e)
      {
         InfoLog(<< "Registration add: contact " << regContact << " was malformed: " << e);
         s << "<p>Error parsing: Contact=" << regContact << "</p>\n";
      }
   }   
   
   s << 
      "<h2>Registrations</h2>" << endl <<
       "<form id=\"showReg\" method=\"get\" action=\"registrations.html\" name=\"showReg\" enctype=\"application/x-www-form-urlencoded\">" << endl << 
      //"<button name=\"removeAllReg\" value=\"\" type=\"button\">Remove All</button>" << endl << 
      //"<hr/>" << endl << 

      "<div class=space>" << endl <<
      "</div>" << endl <<
      "<table" REPRO_BORDERLESS_TABLE_PROPS ">" << endl <<
      "  <tr>" << endl <<
      "    <td align=\"right\">AOR:</td>" << endl <<
      "    <td><input type=\"text\" name=\"regAor\" size=\"40\"/></td>" << endl <<
      "    <td align=\"right\">Contact:</td>" << endl <<
      "    <td><input type=\"text\" name=\"regContact\" size=\"40\"/></td>" << endl <<
      "  </tr>" << endl <<
      "  <tr>" << endl <<
      "    <td align=\"right\">Path:</td>" << endl <<
      "    <td><input type=\"text\" name=\"regPath\" size=\"40\"/></td>" << endl <<
      "    <td></td>" << endl <<
      "    <td align=\"right\"><input type=\"submit\" name=\"action\" value=\"Add\"/></td>" << endl <<
      "  </tr>" << endl <<
      "  </tr>" << endl <<
      "</table>" << endl <<
      "<br>" << endl <<

      "<table" REPRO_BORDERED_TABLE_PROPS ">" << endl << 

      "<tr>" << endl << 
      "  <td>AOR</td>" << endl << 
      "  <td>Contact</td>" << endl << 
      "  <td>Instance ID</td>" << endl <<
      "  <td>Reg ID</td>" << endl <<
      "  <td>QValue</td>" << endl <<
      "  <td>Path</td>" << endl <<
      "  <td>Expires In</td>" << endl << 
      "  <td><input type=\"submit\" value=\"Remove\"/></td>" << endl << 
      "</tr>" << endl;
  
      RegistrationPersistenceManager::UriList aors;
      mRegDb.getAors(aors);
      for ( RegistrationPersistenceManager::UriList::const_iterator 
               aor = aors.begin(); aor != aors.end(); ++aor )
      {
         Uri uri = *aor;
         ContactList contacts;
         mRegDb.getContacts(uri, contacts);
         
         bool first = true;
         UInt64 now = Timer::getTimeSecs();
         for (ContactList::iterator i = contacts.begin();
              i != contacts.end(); ++i )
         {
            if(i->mRegExpires > now)
            {
               UInt64 secondsRemaining = i->mRegExpires - now;

               s << "<tr>" << endl
                 << "  <td>" ;
               if (first) 
               { 
                  s << uri;
                  first = false;
               }
               s << "</td>" << endl
                 << "  <td>";
            
               const ContactInstanceRecord& r = *i;
               const NameAddr& contact = r.mContact;
               const Data& instanceId = r.mInstance;
               int regId = r.mRegId;

               s << contact.uri();
               s <<"</td>" << endl 
                 << "<td>" << instanceId.xmlCharDataEncode() 
                 << "</td><td>" << regId 
                 << "</td><td>";
#ifdef RESIP_FIXED_POINT
               // If RESIP_FIXED_POINT is enabled then q-value is shown as an integer in the range of 0..1000 where 1000 = qvalue of 1.0
               s << (contact.exists(p_q) ? contact.param(p_q) : 1000) << "</td><td>";  
#else
               s << (contact.exists(p_q) ? contact.param(p_q).floatVal() : 1.0f) << "</td><td>";
#endif
               NameAddrs::const_iterator naIt = r.mSipPath.begin();
               for(;naIt != r.mSipPath.end(); naIt++)
               {
                  s << naIt->uri() << "<br>" << endl;
               }
               bool staticRegContact = r.mRegExpires == NeverExpire;
               if(!staticRegContact)
               {
                  s <<"</td><td>" << secondsRemaining << "s</td>" << endl;
               }
               else
               {
                  s <<"</td><td>Never</td>" << endl;
               }
               s << "  <td>"
                 << "<input type=\"checkbox\" name=\"remove." << uri << "\" value=\"" << Data::from(contact.uri()).urlEncoded() 
                                                              << "|" << instanceId.urlEncoded() 
                                                              << "|" << regId
                                                              << "|" << (staticRegContact ? "1" : "0")
                 << "\"/></td>" << endl
                 << "</tr>" << endl;
            }
            else
            {
               // remove expired contact 
               mRegDb.removeContact(uri, *i);
            }
         }
      }
                  
      s << "</table>" << endl << 
         "</form>" << endl;
}


void
WebAdmin::buildSettingsSubPage(DataStream& s)
{
   if (mHttpParams["action"] == "Clear DNS Cache")
   {
      mProxy.getStack().clearDnsCache();
   }

   s << "<h2>Settings</h2>" << endl <<
        "<pre>" << mProxy.getConfig() << "</pre>";

   {
      Data buffer;
      DataStream strm(buffer);
      mProxy.getStack().dump(strm);
      strm.flush();
      s << "<br>Stack Info<br>"
        << "<pre>" <<  buffer << "</pre>"
        << endl;
   }

   if(mProxy.getStack().getCongestionManager())
   {
      Data buffer;
      DataStream strm(buffer);
      mProxy.getStack().getCongestionManager()->encodeCurrentState(strm);
      s << "<br>Congestion Manager Statistics<br>"
        << "<pre>" <<  buffer << "</pre>"
        << endl;
   }

   // Get Dns Cache
   {
      Lock lock(mDnsCacheMutex);
      mProxy.getStack().getDnsCacheDump(make_pair(0, 0), this);
      // Retrieving DNS cache is asyncronous
      // Use condition variable to wait for DNS results to be returned in onDnsCacheDumpRetrieved
      mDnsCacheCondition.wait(mDnsCacheMutex);
      s << "<br>DNS Cache<br>"
        << "<pre>" << mDnsCache << "</pre>"
         << endl;
   }

   s << "<form id=\"clearDnsCache\" method=\"get\" action=\"settings.html\" name=\"clearDnsCache\">" << endl
     << "  <br><input type=\"submit\" name=\"action\" value=\"Clear DNS Cache\"/>" << endl
     << "</form>" << endl;

   if(mProxy.getConfig().getConfigUnsignedShort("CommandPort", 0) != 0)
   {
      s << "<form id=\"restartProxy\" method=\"get\" action=\"restart.html\" name=\"restart\">" << endl
        << "  <input type=\"submit\" name=\"action\" value=\"Restart Proxy\"/>" << endl
        << "</form>" << endl;
   }
}

void 
WebAdmin::onDnsCacheDumpRetrieved(std::pair<unsigned long, unsigned long> key, const resip::Data& dnsEntryStrings)
{
   Lock lock(mDnsCacheMutex); (void)lock;
   if(dnsEntryStrings.empty())
   {
      mDnsCache = "<i>empty</i>";
   }
   else
   {
      mDnsCache = dnsEntryStrings;
   }
   mDnsCacheCondition.signal();
}

void
WebAdmin::buildRestartSubPage(DataStream& s)
{
   unsigned short port = mProxy.getConfig().getConfigUnsignedShort("CommandPort", 0);
   if(port != 0)
   {
      // Send restart command to command server - it is not safe to invoke a restart from here
      // since the webadmin thread and server is destroyed on the blocking ReproRunner::restart call
      int sd, rc;
      struct sockaddr_in localAddr, servAddr;
      struct hostent *h;
      char* host = "127.0.0.1";
      h = gethostbyname(host);
      if(h!=0) 
      {
         servAddr.sin_family = h->h_addrtype;
         memcpy((char *) &servAddr.sin_addr.s_addr, h->h_addr_list[0], h->h_length);
         servAddr.sin_port = htons(port);
  
         // Create TCP Socket
         sd = (int)socket(AF_INET, SOCK_STREAM, 0);
         if(sd > 0) 
         {
            // bind to any local interface/port
            localAddr.sin_family = AF_INET;
            localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
            localAddr.sin_port = 0;

            rc = ::bind(sd, (struct sockaddr *) &localAddr, sizeof(localAddr));
            if(rc >= 0) 
            {
               // Connect to server
               rc = ::connect(sd, (struct sockaddr *) &servAddr, sizeof(servAddr));
               if(rc >= 0) 
               {
                  Data request("<Restart>\r\n  <Request>\r\b  </Request>\r\n</Restart>\r\n");
                  rc = send(sd, request.c_str(), request.size(), 0);
                  if(rc >= 0)
                  {
                     s << "Restarting proxy..." << endl;
                     closeSocket(sd);
                     return;
                  }
               }
            }
            closeSocket(sd);
         }
      }
      s << "Error issuing restart command." << endl;
   }
   else
   {
      s << "CommandServer must be running to use restart feature." << endl;
   }
}

Data 
WebAdmin::buildUserPage()
{ 
   Data ret;
   {
      DataStream s(ret);
      
      s <<  "<?xml version=\"1.0\" encoding=\"utf-8\"?>" << endl
        <<    "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">" << endl
        <<    "" << endl
        <<    "<html xmlns=\"http://www.w3.org/1999/xhtml\">" << endl
        <<    "" << endl
        <<    "<head>" << endl
        <<    "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\" />" << endl
        <<    "<title>Repro Proxy</title>" << endl
        <<    "</head>" << endl
        <<    "" << endl
        <<    "<body bgcolor=\"#ffffff\">" << endl;
      
      //buildAddUserSubPage(s); // !cj! TODO - should do beter page here 
      
      s <<    "</body>" << endl
        <<    "" << endl
        <<    "</html>" << endl;
            
      s.flush();
   }
   return ret;
}


Data
WebAdmin::buildCertPage(const Data& domain)
{
   assert(!domain.empty());
#ifdef USE_SSL
   assert( mProxy.getStack().getSecurity() );
   return mProxy.getStack().getSecurity()->getDomainCertDER(domain);
#else
   ErrLog( << "Proxy not build with support for certificates" );
   return Data::Empty;
#endif
}


Data 
WebAdmin::buildDefaultPage()
{ 
   Data ret;
   {
      DataStream s(ret);
      
      s << 
         "<?xml version=\"1.0\" encoding=\"utf-8\"?>" << endl << 
         "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">" << endl << 
         "<html xmlns=\"http://www.w3.org/1999/xhtml\">" << endl << 
         "<head>" << endl << 
         "<meta http-equiv=\"content-type\" content=\"text/html;charset=utf-8\" />" << endl << 
         "<title>Repro Proxy Login</title>" << endl << 
         "</head>" << endl << 

         "<body bgcolor=\"#ffffff\">" << endl << 
         "  <h1><a href=\"user.html\">Login</a></h1>" << endl << 
         "  <p>The default account is 'admin' with password 'admin', but if you're wise, you've already changed that using the command line</p>" << endl << 
         "</body>" << endl << 
         "</html>" << endl;
      
      s.flush();
   }
   return ret;
}


/* ====================================================================
 * The Vovida Software License, Version 1.0 
 * 
 * Copyright (c) 2000 Vovida Networks, Inc.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 
 * 3. The names "VOCAL", "Vovida Open Communication Application Library",
 *    and "Vovida Open Communication Application Library (VOCAL)" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact vocal@vovida.org.
 *
 * 4. Products derived from this software may not be called "VOCAL", nor
 *    may "VOCAL" appear in their name, without prior written
 *    permission of Vovida Networks, Inc.
 * 
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL VOVIDA
 * NETWORKS, INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT DAMAGES
 * IN EXCESS OF $1,000, NOR FOR ANY INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 * 
 * ====================================================================
 */
