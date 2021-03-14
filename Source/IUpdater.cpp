#include "IUpdater.h"

#include <Windows.h>
#include <wininet.h>
#include "ThirdParty/jsoncpp/json.h"
#include "Config.h"
#include "Utils.h"
#include "ILogger.h"


IUpdater& IUpdater::GetInstance()
{
    static IUpdater i;
    return i;
}

bool IUpdater::CheckUpdate()
{
    auto &Logger = ILogger::GetInstance();

    // Get releases data
    //

    // GitHub REST API limits the rate of unauthenticated requests to 60 per hour
    // See: https://docs.github.com/en/rest/overview/resources-in-the-rest-api#rate-limiting
    //
    // So we first try to request to Google Script API (we call it "bridge" here)
    // It will forward the request to GitHub REST API with authentication information
    //

    std::optional<std::string> Response = GetDataByBridge();
    if (!Response.has_value()) {
        Logger.TraceWarn("[Updater] GetDataByBridge() failed, try GetDataDirectly().");
    }
    else
    {
        if (ParseResponse(Response.value())) {
            Logger.TraceInfo("[Updater] ParseResponse() successed. (ByBridge)");
            return true;
        }
        Logger.TraceWarn("[Updater] ParseResponse() failed, try Directly. (ByBridge)");
    }

    Response = GetDataDirectly();
    if (!Response.has_value()) {
        Logger.TraceWarn("[Updater] GetDataDirectly() failed.");
        return false;
    }

    if (!ParseResponse(Response.value())) {
        Logger.TraceWarn("[Updater] ParseResponse() failed. (Directly)");
        return false;
    }

    Logger.TraceInfo("[Updater] ParseResponse() successed. (Directly)");
    return true;
}

bool IUpdater::ParseResponse(const std::string &Response)
{
    auto &Logger = ILogger::GetInstance();

    // Parse response
    //
    Json::CharReaderBuilder Builder;
    Json::Value Root;
    Json::String Errors;
    std::unique_ptr<Json::CharReader> pReader(Builder.newCharReader());

    if (!pReader->parse(Response.c_str(), Response.c_str() + Response.size(), &Root, &Errors)) {
        Logger.TraceWarn("[Updater] Parse response failed. JsonError: " + Errors + " Response: " + Response);
        return false;
    }

    Json::Value &Message = Root["message"];
    Json::Value &TagName = Root["tag_name"]; // Latest version
    Json::Value &HtmlUrl = Root["html_url"]; // Latest url
    Json::Value &Body = Root["body"];

    if (Message.isString()) {
        Logger.TraceWarn("[Updater] Response has a message. message: " + Message.asString());
    }

    if (!TagName.isString() || !HtmlUrl.isString() || !Body.isString()) {
        Logger.TraceWarn("[Updater] Response fields invalid.");
        return false;
    }

    std::string HtmlUrlContent = HtmlUrl.asString();
    std::string TagNameContent = TagName.asString();
    std::string BodyContent = Body.asString();

    if (HtmlUrlContent.find(AR_REPO_URL) != 0) {
        Logger.TraceWarn("[Updater] html_url field invalid. html_url: " + HtmlUrlContent);
        return false;
    }

    std::vector<std::string> vLocal = Text::SplitByFlag(AR_VERSION_STRING, ".");
    std::vector<std::string> vLatest = Text::SplitByFlag(TagNameContent, ".");

    if (vLocal.size() != 3 || vLatest.size() != 3) {
        Logger.TraceWarn("[Updater] Version format invalid. Local: " AR_VERSION_STRING " Latest: " + TagNameContent);
        return false;
    }

    std::string LocalString = Text::Format("%03d%03d%03d", stoul(vLocal[0]), stoul(vLocal[1]), stoul(vLocal[2]));
    std::string LatestString = Text::Format("%03d%03d%03d", stoul(vLatest[0]), stoul(vLatest[1]), stoul(vLatest[2]));
    uint32_t LocalNumber = stoul(LocalString);
    uint32_t LatestNumber = stoul(LatestString);

    if (LocalNumber >= LatestNumber) {
        Logger.TraceInfo("[Updater] No need to update. Local: " + LocalString + " Latest: " + LatestString);
        return true;
    }

    Logger.TraceInfo("[Updater] Need to update. Local: " + LocalString + " Latest: " + LatestString);

    // Get Changelog
    //

    std::string ChangeLog;
    size_t ClBeginPos = BodyContent.find("Change log");

    if (ClBeginPos != std::string::npos)
    {
        // Find end of ChangeLog
        size_t ClEndPos = BodyContent.find("\r\n\r\n", ClBeginPos), ClCount;

        // If found, calc the size
        if (ClEndPos != std::string::npos) {
            ClCount = ClEndPos - ClBeginPos;
        }
        else {
            ClCount = std::string::npos;
        }

        ChangeLog = BodyContent.substr(ClBeginPos, ClCount) + "\n\n";
    }

    // Pop up the update message
    //

    std::string Msg =
        "A new version has been released.\n"
        "\n"
        "Current version: " AR_VERSION_STRING "\n"
        "Latest version: " + TagNameContent + "\n"
        "\n" +
        ChangeLog +
        "Do you want to go to GitHub to download the latest version?\n";

    if (MessageBoxA(NULL, Msg.c_str(), "Anti-Revoke Plugin", MB_ICONQUESTION | MB_YESNO) == IDYES) {
        system(("start " + HtmlUrlContent).c_str());
    }

    return true;
}

std::optional<std::string> IUpdater::GetDataByBridge()
{
    auto &Logger = ILogger::GetInstance();
    std::optional<std::string> Result = std::nullopt;

    Safe::TryExcept(
        [&]()
        {
            std::string Response;
            uint32_t Status;
            bool IsSuccessed = Internet::HttpRequest(
                Response,
                Status,
                "POST",
                "script.google.com",
                "/macros/s/AKfycbxfGLfG3nXZOIE-t0zFIMGGylBbvj9dc1aiowtAvyh5YEZ69o0/exec",
                {
                    { "Accept" , "application/json" },
                    { "Content-Type" , "application/json" }
                },
                "{\"forward_request\": \"" AR_LATEST_REQUEST "\"}"
            );

            if (!IsSuccessed) {
                Logger.TraceWarn("[Updater] Internet::HttpRequest() failed. (ByBridge)");
                return;
            }

            if (Status != HTTP_STATUS_OK) {
                Logger.TraceWarn("[Updater] Response status is not 200. Status: " + std::to_string(Status) + " Response: " + Response + " (ByBridge)");
                return;
            }

            Json::CharReaderBuilder Builder;
            Json::Value Root;
            Json::String Errors;
            std::unique_ptr<Json::CharReader> pReader(Builder.newCharReader());

            if (!pReader->parse(Response.c_str(), Response.c_str() + Response.size(), &Root, &Errors)) {
                Logger.TraceWarn("[Updater] Parse response failed. JsonError: " + Errors + " Response: " + Response + " (ByBridge)");
                return;
            }

            Json::Value &BridgeErrorMessage = Root["bridge_error_message"];
            if (BridgeErrorMessage.isString()) {
                Logger.TraceWarn("[Updater] bridge_error_message: " + BridgeErrorMessage.asString() + " (ByBridge)");
                return;
            }

            Result = Response;

            Logger.TraceInfo("[Updater] Get data by bridge successed.");
        },
        [&](uint32_t ExceptionCode)
        {
            Logger.TraceWarn("[Updater] An exception was caught. ExceptionCode: " + Text::Format("0x%x", ExceptionCode) + " (ByBridge)");
        }
    );

    return Result;
}

std::optional<std::string> IUpdater::GetDataDirectly()
{
    auto &Logger = ILogger::GetInstance();

    std::string Response;
    uint32_t Status;
    bool IsSuccessed = Internet::HttpRequest(
        Response,
        Status,
        "GET",
        "api.github.com",
        AR_LATEST_REQUEST,
        {
            { "Accept" , "application/vnd.github.v3+json" },
        }
    );

    if (!IsSuccessed) {
        Logger.TraceWarn("[Updater] Internet::HttpRequest() failed. (Directly)");
        return std::nullopt;
    }

    if (Status != HTTP_STATUS_OK) {
        Logger.TraceWarn("[Updater] Response status is not 200. Status: " + std::to_string(Status) + " Response: " + Response + " (Directly)");
        return std::nullopt;
    }

    Logger.TraceInfo("[Updater] Get data directly successed.");
    return Response;
}
