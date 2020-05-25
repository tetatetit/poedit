/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 2015-2020 Vaclav Slavik
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
 *
 */


#include "crowdin_client.h"

#include "http_client.h"
#include "keychain/keytar.h"
#include "str_helpers.h"

#include <functional>
#include <mutex>
#include <stack>
#include <iostream>
#include <ctime>

#include <boost/algorithm/string.hpp>

#include <wx/translation.h>
#include <wx/utils.h>
#include <wx/uri.h>

#include <iostream>

// GCC's libstdc++ didn't have functional std::regex implementation until 4.9
#if (defined(__GNUC__) && !defined(__clang__) && !wxCHECK_GCC_VERSION(4,9))
    #include <boost/regex.hpp>
    using boost::regex;
    using boost::regex_search;
    using boost::smatch;
#else
    #include <regex>
    using std::regex;
    using std::regex_search;
    using std::smatch;
#endif



// ----------------------------------------------------------------
// Implementation
// ----------------------------------------------------------------

namespace
{

#define OAUTH_SCOPE         "project"
#define OAUTH_CLIENT_ID     "k0uFz5HYQh0VzWgZmOpA"
//      any arbitrary unique unguessable string (e.g. UUID in hex)
#define OAUTH_STATE         "948cf13ffffb47119d6cfa2b68898f67"
//      The value of below macro should be set exactly as is (without quotes)
//      to "Authorization Callback URL" of Crowdin application
//      https://support.crowdin.com/enterprise/creating-oauth-app/
#define OAUTH_URI_PREFIX    "poedit://auth/crowdin/"
#define OAUTH_AUTHORIZE_URL "/oauth/authorize" \
    "?" "response_type" "=" "token" \
    "&" "scope"         "=" OAUTH_SCOPE \
    "&" "client_id"     "=" OAUTH_CLIENT_ID \
    "&" "state"         "=" OAUTH_STATE \
    "&" "redirect_uri"  "=" OAUTH_URI_PREFIX \
    "&" "utm_source=poedit.net&utm_medium=referral&utm_campaign=poedit" // source tracking

} // anonymous namespace

std::string CrowdinClient::WrapLink(const std::string& page)
{
    std::string url("https://accounts.crowdin.com");
    if (!page.empty() && page != "/")
        url += page;
    return url;
}


class CrowdinClient::crowdin_http_client : public http_client
{
public:
    crowdin_http_client(CrowdinClient& owner, const std::string& url_prefix)
        : http_client(url_prefix), m_owner(owner)
    {}

protected:
    std::string parse_json_error(const json& response) const override
    {
        wxLogTrace("poedit.crowdin", "JSON error: %s", response.dump().c_str());
        
        try
        {
            return response.at("errors").at(0).at("error").at("errors").at(0).at("message");
        }
        catch(...)
        {
            try
            {
                return response.at("error").at("message");
            }
            catch (...)
            {
                std::string msg;
                msg = _("JSON request error").utf8_str();
                return msg;
            }
        }
    }

    void on_error_response(int& statusCode, std::string& message) override
    {
        if (statusCode == 401/*Unauthorized*/)
        {
            // message is e.g. "The access token provided is invalid"
            message = _("Not authorized, please sign in again.").utf8_str();
            m_owner.SignOut();
        }
        wxLogTrace("poedit.crowdin", "JSON error: %s", message.c_str());
    }

    CrowdinClient& m_owner;
};

CrowdinClient::CrowdinClient() :
    m_oauth(new crowdin_http_client(*this, "https://accounts.crowdin.com"))
{
    SignInIfAuthorized();
}

CrowdinClient::~CrowdinClient() {}


dispatch::future<void> CrowdinClient::Authenticate()
{
    auto url = WrapLink(OAUTH_AUTHORIZE_URL);
    m_authCallback.reset(new dispatch::promise<void>);
    wxLaunchDefaultBrowser(url);
    return m_authCallback->get_future();
}


void CrowdinClient::HandleOAuthCallback(const std::string& uri)
{
    wxLogTrace("poedit.crowdin", "Callback URI %s", uri.c_str());

    smatch m;

    if (!(regex_search(uri, m, regex("state=([^&]+)"))
            && m.size() > 1
            && m.str(1) == OAUTH_STATE))
        return;

    if (!(regex_search(uri, m, regex("access_token=([^&]+)"))
            && m.size() > 1))
        return;

    if (!m_authCallback)
        return;
    SaveAndSetToken(m.str(1));
    m_authCallback->set_value();
    m_authCallback.reset();
}

bool CrowdinClient::IsOAuthCallback(const std::string& uri)
{
    return boost::starts_with(uri, OAUTH_URI_PREFIX);
}

//TODO: validate JSON schema in all API responses
//      and handle errors since now Poedit just crashes
//      in case of some of expected keys missing

dispatch::future<CrowdinClient::UserInfo> CrowdinClient::GetUserInfo()
{
    return m_api->get("user")
        .then([](json r)
        {
            wxLogTrace("poedit.crowdin", "Got user info: %s", r.dump().c_str());
            const json& d = r["data"];
            UserInfo u;
            u.login = str::to_wstring(d["username"]);
            try { u.name = str::to_wstring(d.at("fullName")); }
            catch(...)
            {
                std::string firstName, lastName;
                try { firstName = d.at("firstName").get<std::string>(); } catch(...) {}
                try { lastName = d.at("lastName").get<std::string>(); } catch(...) {}
                u.name = str::to_wstring(firstName + ' ' + lastName);
            }
            if(u.name.empty() || u.name == L" ")
                u.name = u.login;
            return u;
        });
}


dispatch::future<std::vector<CrowdinClient::ProjectListing>> CrowdinClient::GetUserProjects()
{
    // TODO: handle pagination if projects more than 500
    //       (what is quite rare case I guess)
    return m_api->get("projects?limit=500")
        .then([](json r)
        {
            wxLogTrace("poedit.crowdin", "Got projects: %s", r.dump().c_str());
            std::vector<ProjectListing> all;
            for (const auto& d : r["data"])
            {
                const json& i = d["data"];
                auto itPublicDownloads = i.find("publicDownloads");
                if(itPublicDownloads != i.end()
                    // for some wierd reason `publicDownloads` can be in 3 states:
                    // `null`, `true` and `false` and, as investigated experimentally,
                    // only `null` means 'Forbidden' to work with project files (to get list etc)
                    // so hide such projects. While `false` or `true` allows to work with 
                    // such projects normally
                    && !itPublicDownloads->is_null()) {

                    all.push_back({
                        str::to_wstring(i["name"]),
                        i["id"].get<int>()
                });

                }
            }
            return all;
        });
}


dispatch::future<CrowdinClient::ProjectInfo> CrowdinClient::GetProjectInfo(const int project_id)
{
    auto url = "projects/" + std::to_string(project_id);
    auto prj = std::make_shared<ProjectInfo>();
    enum { NO_ID = -1 };
        
    return m_api->get(url)
    .then([this, url, prj](json r)
    {
        // Handle project info
        const json& d = r["data"];
        prj->name = str::to_wstring(d["name"]);
        prj->id = d["id"];
        for (const auto& langCode : d["targetLanguageIds"])
            prj->languages.push_back(Language::TryParse(str::to_wstring(langCode)));
        //TODO: get more until all files gotten (if more than 500)
        return m_api->get(url + "/files?limit=500");
    }).then([this, url, prj](json r)
    {   
        // Handle project files
        for(auto& i : r["data"])
        {
            const json& d = i["data"];
            if(d["type"] != "assets")
            {
                const json& dir = d["directoryId"],
                            branch = d["branchId"];
                prj->files.push_back({
                    L"/" + str::to_wstring(d["name"]),
                    d["id"],
                    dir.is_null() ? NO_ID : dir.get<int>(),
                    branch.is_null() ? NO_ID : branch.get<int>()
                });
            }
        }
        //TODO: get more until all dirs gotten (if more than 500)
        return m_api->get(url + "/directories?limit=500");
    }).then([this, url, prj](json r)
    {
        // Handle directories
        struct dir {
            std::string name;
            int parentId;
        };
        std::map<int, dir> dirs;

        for(const auto& i : r["data"])
        {
            const json& d = i["data"],
                  parent = d["directoryId"];
            dirs.insert({
                d["id"],
                {
                    d["name"],
                    parent.is_null() ? NO_ID : parent.get<int>()
                }
            });
        }

        std::stack<std::string> path;
        for(auto& i : prj->files)
        {
            int dirId = i.dirId;
            while(dirId != NO_ID)
            {
                const auto& dir = dirs[dirId];
                path.push(dir.name);
                dirId = dir.parentId;
            }
            std::string pathStr;
            while(path.size())
            {
                pathStr += '/';
                pathStr += path.top();
                path.pop();
            }
            if(!pathStr.empty())
                i.pathName = str::to_wstring(pathStr) + i.pathName;
        }

        //TODO: get more until all branches gotten (if more than 500)    
        return m_api->get(url + "/branches?limit=500");
    }).then([this, url, prj](json r)
    {
        // Handle branches
        std::map<int, std::string> branches;

        for(const auto& i : r["data"])
        {
            const json& d = i["data"];
            branches[d["id"]] = d["name"].get<std::string>();
        }

        for(auto& i : prj->files)
            if(i.branchId != NO_ID)
                i.pathName = "/" + str::to_wstring(branches[i.branchId]) + i.pathName;

        return *prj;
    });
}


dispatch::future<void> CrowdinClient::DownloadFile(const long project_id,
                                                   const Language& lang,
                                                   const long file_id,
                                                   const std::string& file_extension,
                                                   const std::wstring& output_file)
{
    wxLogTrace("poedit.crowdin", "Getting file URL: projects/%ld/translations/builds/files/%ld", project_id, file_id);
    wxString ext(file_extension);
    ext.MakeLower();
    return m_api->post(
        "projects/" + std::to_string(project_id) + "/translations/builds/files/" + std::to_string(file_id),
        json_data({
            { "targetLanguageId", lang.LanguageTag() },
            // for XLIFF and PO files should be exported "as is" so set to `false`
            { "exportAsXliff", !(ext == "xliff" || ext == "po") }
        }))
        .then([this, output_file] (json r) {
            std::string url = r["data"]["url"];
            wxURI uri(url);
            // Per download (local) client must be created since different domain
            // per request is not allowed by HTTP client backend on some platorms.
            // (e.g. on Linux).
            auto downloader = std::make_shared<crowdin_http_client>(*this, std::string((uri.GetScheme() + "://" + uri.GetServer()).mb_str()));
            wxLogTrace("poedit.crowdin", "Gotten file URL: %s", r.dump().c_str());
            // Below capturing of `[downloader]` is needed to preserve `downloader` object
            // from being destroyed before `download(...)` completes asynchroneously
            // (what happens already after current function returns)
            return downloader->download(url, output_file).then([downloader]() {});
        });
}


dispatch::future<void> CrowdinClient::UploadFile(const long project_id,
                                                 const Language& lang,
                                                 const long file_id,
                                                 const std::string& file_extension,
                                                 const std::string& file_content)
{
    return m_api->post(
            "storages",
            octet_stream_data(file_content),
            { { "Crowdin-API-FileName", "crowdin." + file_extension } }
        )
        .then([this, project_id, file_id, lang] (json r) {
            wxLogTrace("poedit.crowdin", "File uploaded to temporary storage: %s", r.dump().c_str());
            const auto storageId = r["data"]["id"].get<int>();
            return m_api->post(
                "projects/" + std::to_string(project_id) + "/translations/" + lang.LanguageTag(),
                json_data({
                    { "storageId", storageId },
                    { "fileId", file_id },
                    { "importDuplicates", true }
                }))
                .then([](json r) {
                    wxLogTrace("poedit.crowdin", "File uploaded: %s", r.dump().c_str());
                });
        });
}


bool CrowdinClient::IsSignedIn() const
{
    std::string token;
    return keytar::GetPassword("Crowdin", "", &token);
}


void CrowdinClient::SignInIfAuthorized()
{
    std::string token;
    if (keytar::GetPassword("Crowdin", "", &token))
        SetToken(token);
    wxLogTrace("poedit.crowdin", "Token: %s", token.c_str());
}


static std::string base64_decode_json_part(const std::string &in) {

    std::string out;
    std::vector<int> t(256, -1);
    {
        static const char* b = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i ++)
            t[b[i]] = i;
    }

    int val = 0,
        valb = -8;

    for (uint8_t c : in)
    {
        if (t[c] == -1)
            break;
        val = (val << 6) + t[c];
        valb += 6;
        if (valb >= 0)
        {
            out.push_back(char(val >> valb & 0xFF));
            valb -= 8;
        }
    }

    return out;
}


void CrowdinClient::SetToken(const std::string& token)
{
    wxLogTrace("poedit.crowdin", "Authorization: %s", token.c_str());

    if(token.empty())
        return;

    std::string domain;
    try
    {
        domain = json::parse(
            base64_decode_json_part(std::string(
                wxString(token).AfterFirst('.').BeforeFirst('.').utf8_str()
        ))).at("domain").get<std::string>();
        domain += ".";
    }
    catch(...) {}
    m_api = std::make_unique<crowdin_http_client>(*this, "https://" + domain + "crowdin.com/api/v2");
    m_api->set_authorization("Bearer " + token);
}


void CrowdinClient::SaveAndSetToken(const std::string& token)
{
    SetToken(token);
    keytar::AddPassword("Crowdin", "", token);
}


void CrowdinClient::SignOut()
{
    m_api->set_authorization("");
    keytar::DeletePassword("Crowdin", "");
}


// ----------------------------------------------------------------
// Singleton management
// ----------------------------------------------------------------

static std::once_flag initializationFlag;
CrowdinClient* CrowdinClient::ms_instance = nullptr;

CrowdinClient& CrowdinClient::Get()
{
    std::call_once(initializationFlag, []() {
        ms_instance = new CrowdinClient;
    });
    return *ms_instance;
}

void CrowdinClient::CleanUp()
{
    if (ms_instance)
    {
        delete ms_instance;
        ms_instance = nullptr;
    }
}

