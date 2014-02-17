/*
    PowerDNS Versatile Database Driven Nameserver
    Copyright (C) 2003 - 2012  PowerDNS.COM BV

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation

    Additionally, the license of this program contains a special
    exception which allows to distribute the program in binary form when
    it is linked against OpenSSL.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include "ws-recursor.hh"
#include "json.hh"
#include <boost/foreach.hpp>
#include <string>
#include "namespaces.hh"
#include <iostream>
#include "iputils.hh"
#include "rec_channel.hh"
#include "arguments.hh"
#include "misc.hh"
#include "syncres.hh"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "webserver.hh"
#include "ws-api.hh"
#include "logger.hh"

using namespace rapidjson;

void productServerStatisticsFetch(map<string,string>& out)
{
  map<string,string> stats = getAllStatsMap();
  out.swap(stats);
}

static void apiWriteConfigFile(const string& filebasename, const string& content)
{
  if (::arg()["experimental-api-config-dir"].empty()) {
    throw ApiException("Config Option \"experimental-api-config-dir\" must be set");
  }

  string filename = ::arg()["experimental-api-config-dir"] + "/" + filebasename + ".conf";
  ofstream ofconf(filename.c_str());
  if (!ofconf) {
    throw ApiException("Could not open config fragment file '"+filename+"' for writing: "+stringerror());
  }
  ofconf << "# Generated by pdns-recursor REST API, DO NOT EDIT" << endl;
  ofconf << content << endl;
  ofconf.close();
}

static void apiServerConfigAllowFrom(HttpRequest* req, HttpResponse* resp)
{
  if (req->method == "PUT") {
    Document document;
    req->json(document);
    const Value &jlist = document["value"];

    if (!document.IsObject()) {
      throw ApiException("Supplied JSON must be an object");
    }

    if (!jlist.IsArray()) {
      throw ApiException("'value' must be an array");
    }

    ostringstream ss;

    // Clear allow-from-file if set, so our changes take effect
    ss << "allow-from-file=" << endl;

    // Clear allow-from, and provide a "parent" value
    ss << "allow-from=" << endl;
    for (SizeType i = 0; i < jlist.Size(); ++i) {
      ss << "allow-from+=" << jlist[i].GetString() << endl;
    }

    apiWriteConfigFile("allow-from", ss.str());

    parseACLs();

    // fall through to GET
  } else if (req->method != "GET") {
    throw HttpMethodNotAllowedException();
  }

  // Return currently configured ACLs
  Document document;
  document.SetObject();

  Value jlist;
  jlist.SetArray();

  vector<string> entries;
  t_allowFrom->toStringVector(&entries);

  BOOST_FOREACH(const string& entry, entries) {
    Value jentry(entry.c_str(), document.GetAllocator()); // copy
    jlist.PushBack(jentry, document.GetAllocator());
  }

  document.AddMember("name", "allow-from", document.GetAllocator());
  document.AddMember("value", jlist, document.GetAllocator());

  resp->setBody(document);
}

static void fillZone(const string& zonename, HttpResponse* resp)
{
  SyncRes::domainmap_t::const_iterator iter = t_sstorage->domainmap->find(zonename);
  if (iter == t_sstorage->domainmap->end())
    throw ApiException("Could not find domain '"+zonename+"'");

  Document doc;
  doc.SetObject();

  const SyncRes::AuthDomain& zone = iter->second;

  // id is the canonical lookup key, which doesn't actually match the name (in some cases)
  string zoneId = apiZoneNameToId(iter->first);
  Value jzoneid(zoneId.c_str(), doc.GetAllocator()); // copy
  doc.AddMember("id", jzoneid, doc.GetAllocator());
  string url = "/servers/localhost/zones/" + zoneId;
  Value jurl(url.c_str(), doc.GetAllocator()); // copy
  doc.AddMember("url", jurl, doc.GetAllocator());
  doc.AddMember("name", iter->first.c_str(), doc.GetAllocator());
  doc.AddMember("kind", zone.d_servers.empty() ? "Native" : "Forwarded", doc.GetAllocator());
  Value servers;
  servers.SetArray();
  BOOST_FOREACH(const ComboAddress& server, zone.d_servers) {
    Value value(server.toStringWithPort().c_str(), doc.GetAllocator());
    servers.PushBack(value, doc.GetAllocator());
  }
  doc.AddMember("servers", servers, doc.GetAllocator());
  bool rd = zone.d_servers.empty() ? false : zone.d_rdForward;
  doc.AddMember("recursion_desired", rd, doc.GetAllocator());

  Value records;
  records.SetArray();
  BOOST_FOREACH(const SyncRes::AuthDomain::records_t::value_type& rr, zone.d_records) {
    Value object;
    object.SetObject();
    Value jname(rr.qname.c_str(), doc.GetAllocator()); // copy
    object.AddMember("name", jname, doc.GetAllocator());
    Value jtype(rr.qtype.getName().c_str(), doc.GetAllocator()); // copy
    object.AddMember("type", jtype, doc.GetAllocator());
    object.AddMember("ttl", rr.ttl, doc.GetAllocator());
    object.AddMember("priority", rr.priority, doc.GetAllocator());
    Value jcontent(rr.content.c_str(), doc.GetAllocator()); // copy
    object.AddMember("content", jcontent, doc.GetAllocator());
    records.PushBack(object, doc.GetAllocator());
  }
  doc.AddMember("records", records, doc.GetAllocator());

  resp->setBody(doc);
}

static void doCreateZone(const Value& document)
{
  if (::arg()["experimental-api-config-dir"].empty()) {
    throw ApiException("Config Option \"experimental-api-config-dir\" must be set");
  }

  string zonename = stringFromJson(document, "name");
  // TODO: better validation of zonename
  if(zonename.empty())
    throw ApiException("Zone name empty");

  if (zonename[zonename.size()-1] != '.') {
    zonename += ".";
  }

  string kind = toUpper(stringFromJson(document, "kind"));
  bool rd = boolFromJson(document, "recursion_desired");
  string confbasename = "zone-" + apiZoneNameToId(zonename);

  if (kind == "NATIVE") {
    if (rd)
      throw ApiException("kind=Native and recursion_desired are mutually exclusive");

    string zonefilename = ::arg()["experimental-api-config-dir"] + "/" + confbasename + ".zone";
    ofstream ofzone(zonefilename.c_str());
    if (!ofzone) {
      throw ApiException("Could not open '"+zonefilename+"' for writing: "+stringerror());
    }
    ofzone << "; Generated by pdns-recursor REST API, DO NOT EDIT" << endl;
    ofzone << zonename << "\tIN\tSOA\tlocal.zone.\thostmaster."<<zonename<<" 1 1 1 1 1" << endl;
    ofzone.close();

    apiWriteConfigFile(confbasename, "auth-zones+=" + zonename + "=" + zonefilename);
  } else if (kind == "FORWARDED") {
    const Value &servers = document["servers"];
    if (kind == "FORWARDED" && (!servers.IsArray() || servers.Size() == 0))
      throw ApiException("Need at least one upstream server when forwarding");

    string serverlist;
    if (servers.IsArray()) {
      for (SizeType i = 0; i < servers.Size(); ++i) {
        if (!serverlist.empty()) {
          serverlist += ";";
        }
        serverlist += servers[i].GetString();
      }
    }

    if (rd) {
      apiWriteConfigFile(confbasename, "forward-zones-recurse+=" + zonename + "=" + serverlist);
    } else {
      apiWriteConfigFile(confbasename, "forward-zones+=" + zonename + "=" + serverlist);
    }
  } else {
    throw ApiException("invalid kind");
  }
}

static bool doDeleteZone(const string& zonename)
{
  if (::arg()["experimental-api-config-dir"].empty()) {
    throw ApiException("Config Option \"experimental-api-config-dir\" must be set");
  }

  string filename;

  // this one must exist
  filename = ::arg()["experimental-api-config-dir"] + "/zone-" + apiZoneNameToId(zonename) + ".conf";
  if (unlink(filename.c_str()) != 0) {
    return false;
  }

  // .zone file is optional
  filename = ::arg()["experimental-api-config-dir"] + "/zone-" + apiZoneNameToId(zonename) + ".zone";
  unlink(filename.c_str());

  return true;
}

static void apiServerZones(HttpRequest* req, HttpResponse* resp)
{
  if (req->method == "POST") {
    if (::arg()["experimental-api-config-dir"].empty()) {
      throw ApiException("Config Option \"experimental-api-config-dir\" must be set");
    }

    Document document;
    req->json(document);

    string zonename = stringFromJson(document, "name");
    if (zonename[zonename.size()-1] != '.') {
      zonename += ".";
    }

    SyncRes::domainmap_t::const_iterator iter = t_sstorage->domainmap->find(zonename);
    if (iter != t_sstorage->domainmap->end())
      throw ApiException("Zone already exists");

    doCreateZone(document);
    reloadAuthAndForwards();
    fillZone(zonename, resp);
    return;
  }

  if(req->method != "GET")
    throw HttpMethodNotAllowedException();

  Document doc;
  doc.SetArray();

  BOOST_FOREACH(const SyncRes::domainmap_t::value_type& val, *t_sstorage->domainmap) {
    const SyncRes::AuthDomain& zone = val.second;
    Value jdi;
    jdi.SetObject();
    // id is the canonical lookup key, which doesn't actually match the name (in some cases)
    string zoneId = apiZoneNameToId(val.first);
    Value jzoneid(zoneId.c_str(), doc.GetAllocator()); // copy
    jdi.AddMember("id", jzoneid, doc.GetAllocator());
    string url = "/servers/localhost/zones/" + zoneId;
    Value jurl(url.c_str(), doc.GetAllocator()); // copy
    jdi.AddMember("url", jurl, doc.GetAllocator());
    jdi.AddMember("name", val.first.c_str(), doc.GetAllocator());
    jdi.AddMember("kind", zone.d_servers.empty() ? "Native" : "Forwarded", doc.GetAllocator());
    Value servers;
    servers.SetArray();
    BOOST_FOREACH(const ComboAddress& server, zone.d_servers) {
      Value value(server.toStringWithPort().c_str(), doc.GetAllocator());
      servers.PushBack(value, doc.GetAllocator());
    }
    jdi.AddMember("servers", servers, doc.GetAllocator());
    bool rd = zone.d_servers.empty() ? false : zone.d_rdForward;
    jdi.AddMember("recursion_desired", rd, doc.GetAllocator());
    doc.PushBack(jdi, doc.GetAllocator());
  }
  resp->setBody(doc);
}

static void apiServerZoneDetail(HttpRequest* req, HttpResponse* resp)
{
  string zonename = apiZoneIdToName(req->path_parameters["id"]);
  zonename += ".";

  SyncRes::domainmap_t::const_iterator iter = t_sstorage->domainmap->find(zonename);
  if (iter == t_sstorage->domainmap->end())
    throw ApiException("Could not find domain '"+zonename+"'");

  if(req->method == "PUT") {
    Document document;
    req->json(document);

    doDeleteZone(zonename);
    doCreateZone(document);
    reloadAuthAndForwards();
    fillZone(stringFromJson(document, "name"), resp);
  }
  else if(req->method == "DELETE") {
    if (!doDeleteZone(zonename)) {
      throw ApiException("Deleting domain failed");
    }

    reloadAuthAndForwards();
    // empty body on success
    resp->body = "";
  } else if(req->method == "GET") {
    fillZone(zonename, resp);
  } else {
    throw HttpMethodNotAllowedException();
  }
}

RecursorWebServer::RecursorWebServer(FDMultiplexer* fdm)
{
  RecursorControlParser rcp; // inits

  if(!arg().mustDo("experimental-webserver")) {
    d_ws = NULL;
    return;
  }

  d_ws = new AsyncWebServer(fdm, arg()["experimental-webserver-address"], arg().asNum("experimental-webserver-port"), arg()["experimental-webserver-password"]);

  // legacy dispatch
  d_ws->registerApiHandler("/jsonstat", boost::bind(&RecursorWebServer::jsonstat, this, _1, _2));
  d_ws->registerApiHandler("/servers/localhost/config/allow-from", &apiServerConfigAllowFrom);
  d_ws->registerApiHandler("/servers/localhost/config", &apiServerConfig);
  d_ws->registerApiHandler("/servers/localhost/search-log", &apiServerSearchLog);
  d_ws->registerApiHandler("/servers/localhost/statistics", &apiServerStatistics);
  d_ws->registerApiHandler("/servers/localhost/zones/<id>", &apiServerZoneDetail);
  d_ws->registerApiHandler("/servers/localhost/zones", &apiServerZones);
  d_ws->registerApiHandler("/servers/localhost", &apiServerDetail);
  d_ws->registerApiHandler("/servers", &apiServer);

  d_ws->go();
}

void RecursorWebServer::jsonstat(HttpRequest* req, HttpResponse *resp)
{
  string command;

  if(req->parameters.count("command")) {
    command = req->parameters["command"];
    req->parameters.erase("command");
  }

  map<string, string> stats; 
  if(command == "domains") {
    Document doc;
    doc.SetArray();
    BOOST_FOREACH(const SyncRes::domainmap_t::value_type& val, *t_sstorage->domainmap) {
      Value jzone;
      jzone.SetObject();

      const SyncRes::AuthDomain& zone = val.second;
      Value zonename(val.first.c_str(), doc.GetAllocator());
      jzone.AddMember("name", zonename, doc.GetAllocator());
      jzone.AddMember("type", "Zone", doc.GetAllocator());
      jzone.AddMember("kind", zone.d_servers.empty() ? "Native" : "Forwarded", doc.GetAllocator());
      Value servers;
      servers.SetArray();
      BOOST_FOREACH(const ComboAddress& server, zone.d_servers) {
        Value value(server.toStringWithPort().c_str(), doc.GetAllocator());
        servers.PushBack(value, doc.GetAllocator());
      }
      jzone.AddMember("servers", servers, doc.GetAllocator());
      bool rdbit = zone.d_servers.empty() ? false : zone.d_rdForward;
      jzone.AddMember("rdbit", rdbit, doc.GetAllocator());

      doc.PushBack(jzone, doc.GetAllocator());
    }
    resp->setBody(doc);
    return;
  }
  else if(command == "zone") {
    string arg_zone = req->parameters["zone"];
    SyncRes::domainmap_t::const_iterator ret = t_sstorage->domainmap->find(arg_zone);
    if (ret != t_sstorage->domainmap->end()) {
      Document doc;
      doc.SetObject();
      Value root;
      root.SetObject();

      const SyncRes::AuthDomain& zone = ret->second;
      Value zonename(ret->first.c_str(), doc.GetAllocator());
      root.AddMember("name", zonename, doc.GetAllocator());
      root.AddMember("type", "Zone", doc.GetAllocator());
      root.AddMember("kind", zone.d_servers.empty() ? "Native" : "Forwarded", doc.GetAllocator());
      Value servers;
      servers.SetArray();
      BOOST_FOREACH(const ComboAddress& server, zone.d_servers) {
        Value value(server.toStringWithPort().c_str(), doc.GetAllocator());
        servers.PushBack(value, doc.GetAllocator());
      }
      root.AddMember("servers", servers, doc.GetAllocator());
      bool rdbit = zone.d_servers.empty() ? false : zone.d_rdForward;
      root.AddMember("rdbit", rdbit, doc.GetAllocator());

      Value records;
      records.SetArray();
      BOOST_FOREACH(const SyncRes::AuthDomain::records_t::value_type& rr, zone.d_records) {
        Value object;
        object.SetObject();
        Value jname(rr.qname.c_str(), doc.GetAllocator()); // copy
        object.AddMember("name", jname, doc.GetAllocator());
        Value jtype(rr.qtype.getName().c_str(), doc.GetAllocator()); // copy
        object.AddMember("type", jtype, doc.GetAllocator());
        object.AddMember("ttl", rr.ttl, doc.GetAllocator());
        object.AddMember("priority", rr.priority, doc.GetAllocator());
        Value jcontent(rr.content.c_str(), doc.GetAllocator()); // copy
        object.AddMember("content", jcontent, doc.GetAllocator());
        records.PushBack(object, doc.GetAllocator());
      }
      root.AddMember("records", records, doc.GetAllocator());

      doc.AddMember("zone", root, doc.GetAllocator());
      resp->setBody(doc);
      return;
    } else {
      resp->body = returnJsonError("Could not find domain '"+arg_zone+"'");
      return;
    }
  }
  else if(command == "flush-cache") {
    string canon=toCanonic("", req->parameters["domain"]);
    int count = broadcastAccFunction<uint64_t>(boost::bind(pleaseWipeCache, canon));
    count+=broadcastAccFunction<uint64_t>(boost::bind(pleaseWipeAndCountNegCache, canon));
    stats["number"]=lexical_cast<string>(count);
    resp->body = returnJsonObject(stats);
    return;
  }
  else if(command == "config") {
    vector<string> items = ::arg().list();
    BOOST_FOREACH(const string& var, items) {
      stats[var] = ::arg()[var];
    }
    resp->body = returnJsonObject(stats);
    return;
  }
  else if(command == "log-grep") {
    // legacy parameter name hack
    req->parameters["q"] = req->parameters["needle"];
    apiServerSearchLog(req, resp);
    return;
  }
  else if(command == "stats") {
    stats = getAllStatsMap();
    resp->body = returnJsonObject(stats);
    return;
  } else {
    resp->status = 404;
    resp->body = returnJsonError("Not found");
  }
}
