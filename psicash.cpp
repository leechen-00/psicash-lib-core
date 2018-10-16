#include <map>
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>
#include "psicash.h"
#include "userdata.h"
#include "datetime.h"
#include "error.h"
#include "url.h"
#include "base64.h"
#include "http_status_codes.h"

#include "nlohmann/json.hpp"

using json = nlohmann::json;

using namespace std;
using namespace nonstd;
using namespace psicash;
using namespace error;

namespace psicash {

static constexpr const char* kAPIServerScheme = "https";
static constexpr const char* kAPIServerHostname = "dev-api.psi.cash";
static constexpr int kAPIServerPort = 443;
static constexpr const char* kAPIServerVersion = "v1";
static constexpr const char* kPsiCashUserAgent = "Psiphon-PsiCash-iOS"; // TODO: CAN'T HARDCODE -- PLATFORM DEPENDENT
static const string kLandingPageParamKey = "psicash";
static constexpr const char* kMethodGET = "GET";
static constexpr const char* kMethodPOST = "POST";


string ErrorMsg(const string& message, const string& filename, const string& function, int line) {
    Error err(message, filename, function, line);
    return string("{\"status\":-1,\"error\":\"") + err.ToString() + "\"}";
}

string ErrorMsg(const Error& error, const string& message, const string& filename, const string& function, int line) {
    Error wrapping_err(error);
    wrapping_err.Wrap(message, filename, function, line);
    return string("{\"status\":-1,\"error\":\"") + wrapping_err.ToString() + "\"}";
}

//
// PsiCash class implementation
//

PsiCash::PsiCash()
        : make_http_request_fn_(nullptr) {
}

PsiCash::~PsiCash() {
}

Error PsiCash::Init(const char* file_store_root, MakeHTTPRequestFn make_http_request_fn) {
    if (!file_store_root) {
        return MakeError("file_store_root null");
    }

    make_http_request_fn_ = make_http_request_fn;

    user_data_ = std::make_unique<UserData>();
    auto err = user_data_->Init(file_store_root);
    if (err) {
        // If UserData.Init fails, the only way to proceed to try to reset it and create a new one.
        user_data_->Clear();
        err = user_data_->Init(file_store_root);
        if (err) {
            return PassError(err);
        }
    }

    return nullerr;
}

void PsiCash::SetHTTPRequestFn(MakeHTTPRequestFn make_http_request_fn) {
    make_http_request_fn_ = make_http_request_fn;
}

Error PsiCash::SetRequestMetadataItem(const string& key, const string& value) {
    return PassError(user_data_->SetRequestMetadataItem(key, value));
}

//
// Stored info accessors
//

bool PsiCash::IsAccount() const {
    return user_data_->GetIsAccount();
}

TokenTypes PsiCash::ValidTokenTypes() const {
    TokenTypes tt;

    auto auth_tokens = user_data_->GetAuthTokens();
    for (const auto& it : auth_tokens) {
        tt.push_back(it.first);
    }

    return tt;
}

int64_t PsiCash::Balance() const {
    return user_data_->GetBalance();
}

PurchasePrices PsiCash::GetPurchasePrices() const {
    return user_data_->GetPurchasePrices();
}

Purchases PsiCash::GetPurchases() const {
    return user_data_->GetPurchases();
}

static bool IsExpired(const Purchase& p) {
    auto local_now = datetime::DateTime::Now();
    return (p.local_time_expiry && *p.local_time_expiry < local_now);
}

Purchases PsiCash::ValidPurchases() const {
    Purchases res;
    for (const auto& p : user_data_->GetPurchases()) {
        if (!IsExpired(p)) {
            res.push_back(p);
        }
    }
    return res;
}

optional<Purchase> PsiCash::NextExpiringPurchase() const {
    optional<Purchase> next;
    for (const auto& p : user_data_->GetPurchases()) {
        // We're using server time, since we're not comparing to local now.
        if (!p.server_time_expiry) {
            continue;
        }

        if (!next) {
            // We haven't yet set a next.
            next = p;
            continue;
        }

        if (p.server_time_expiry < next->server_time_expiry) {
            next = p;
        }
    }

    return next;
}

Result<Purchases> PsiCash::ExpirePurchases() {
    auto all_purchases = GetPurchases();
    Purchases expired_purchases, valid_purchases;
    for (const auto& p : all_purchases) {
        if (IsExpired(p)) {
            expired_purchases.push_back(p);
        } else {
            valid_purchases.push_back(p);
        }
    }

    auto err = user_data_->SetPurchases(valid_purchases);
    if (err) {
        return WrapError(err, "SetPurchases failed");
    }

    return expired_purchases;
}

Error PsiCash::RemovePurchases(const vector<TransactionID>& ids) {
    auto all_purchases = GetPurchases();
    Purchases remaining_purchases;
    for (const auto& p : all_purchases) {
        bool match = false;
        for (const auto& id : ids) {
            if (p.id == id) {
                match = true;
                break;
            }
        }

        if (!match) {
            remaining_purchases.push_back(p);
        }
    }

    auto err = user_data_->SetPurchases(remaining_purchases);
    return WrapError(err, "SetPurchases failed");
}

Result<string> PsiCash::ModifyLandingPage(const string& url_string) const {
    URL url;
    auto err = url.Parse(url_string);
    if (err) {
        return WrapError(err, "url.Parse failed");
    }

    json psicash_data;
    psicash_data["v"] = 1;

    auto auth_tokens = user_data_->GetAuthTokens();
    if (auth_tokens.size() == 0) {
        psicash_data["tokens"] = nullptr;
    } else {
        psicash_data["tokens"] = auth_tokens[kEarnerTokenType];
    }

    // Get the metadata (sponsor ID, etc.)
    psicash_data["metadata"] = user_data_->GetRequestMetadata();

    string json_data;
    try {
        json_data = psicash_data.dump(-1, ' ', true);
    }
    catch (json::exception& e) {
        return MakeError(
                utils::Stringer("json dump failed: ", e.what(), "; id:", e.id).c_str());
    }

    // Our preference is to put the our data into the URL's fragment/hash/anchor,
    // because we'd prefer the data not be sent to the server.
    // But if there already is a fragment value then we'll put our data into the query parameters.
    // (Because altering the fragment is more likely to have negative consequences
    // for the page than adding a query parameter that will be ignored.)

    if (url.fragment_.empty()) {
        url.fragment_ = kLandingPageParamKey + "=" + URL::Encode(json_data, true);
    } else {
        if (!url.query_.empty()) {
            url.query_ += "&";
        }
        url.query_ += kLandingPageParamKey + "=" + URL::Encode(json_data, true);
    }

    return url.ToString();
}

Result<string> PsiCash::GetRewardedActivityData() const {
    /*
     The data is base64-encoded JSON-serialized with this structure:
     {
         "v": 1,
         "tokens": "earner token",
         "metadata": {
             "client_region": "CA",
             "client_version": "123",
             "sponsor_id": "ABCDEFGH12345678",
             "propagation_channel_id": "ABCDEFGH12345678"
         },
         "user_agent": "PsiCash-iOS-Client"
     }
    */

    json psicash_data;
    psicash_data["v"] = 1;

    // Get the earner token. If we don't have one, the webhook can't succeed.
    auto auth_tokens = user_data_->GetAuthTokens();
    if (auth_tokens.size() == 0) {
        return MakeError("earner token missing; can't create webhoook data");
    } else {
        psicash_data["tokens"] = auth_tokens[kEarnerTokenType];
    }

    // Get the metadata (sponsor ID, etc.)
    psicash_data["metadata"] = user_data_->GetRequestMetadata();

    string json_data;
    try {
        json_data = psicash_data.dump(-1, ' ', true);
    }
    catch (json::exception& e) {
        return MakeError(
                utils::Stringer("json dump failed: ", e.what(), "; id:", e.id).c_str());
    }

    json_data = base64::B64Encode(json_data);

    return json_data;
}

json PsiCash::GetDiagnosticInfo() const {
    json j = json::object();

    j["validTokenTypes"] = ValidTokenTypes();
    j["isAccount"] = IsAccount();
    j["balance"] = Balance();
    j["serverTimeDiff"] = user_data_->GetServerTimeDiff().count();
    j["purchasePrices"] = GetPurchasePrices();

    // Include a sanitized version of the purchases
    j["purchases"] = json::array();
    for (const auto& p : GetPurchases()) {
        j["purchases"].push_back({{"class",         p.transaction_class},
                                  {"distinguisher", p.distinguisher}});
    }

    return j;
}

//
// API Server Requests
//

Result<HTTPResult> PsiCash::MakeHTTPRequestWithRetry(
        const std::string& method, const std::string& path,
        bool include_auth_tokens, const nlohmann::json& query_params) {
    const int max_attempts = 3;
    HTTPResult http_result;

    for (int i = 0; i < max_attempts; i++) {
        if (i < 0) {
            // Not the first attempt; wait before retrying
            this_thread::sleep_for(chrono::seconds(i));
        }

        auto req_params = BuildRequestParams(method, path, include_auth_tokens, query_params,
                                             i + 1);
        if (!req_params) {
            return WrapError(req_params.error(), "BuildRequestParams failed");
        }

        auto result_string = make_http_request_fn_(*req_params);
        if (result_string.empty()) {
            // An error so catastrophic that we don't get any error info.
            return MakeError("HTTP request function returned no value");
        }

        try {
            auto j = json::parse(result_string);
            http_result.status = j["status"].get<int>();

            if (j["body"].is_string()) {
                http_result.body = j["body"].get<string>();
            }

            if (j["date"].is_string()) {
                http_result.date = j["date"].get<string>();
            }

            if (j["error"].is_string()) {
                http_result.error = j["error"].get<string>();
            }
        }
        catch (json::exception& e) {
            return MakeError(
                    utils::Stringer("json parse failed: ", e.what(), "; id:", e.id).c_str());
        }

        if (http_result.status == -1 && http_result.error.empty()) {
            return MakeError("HTTP result status is -1 but no error message provided");
        }

        // We just got a fresh server timestamp, so set the server time diff
        if (!http_result.date.empty()) {
            datetime::DateTime server_datetime;
            if (server_datetime.FromRFC7231(http_result.date)) {
                // We don't care about the return value at this point.
                (void) user_data_->SetServerTimeDiff(server_datetime);
            }
            // else: we're not going to raise the error
        }

        if (!http_result.error.empty()) {
            // Something happened that prevented the request from nominally succeeding. Don't retry.
            return MakeError(("Request resulted in error: "s + http_result.error).c_str());
        }

        if (http_result.status >= 500) {
            // Server error; retry
            continue;
        }

        // We got a response of less than 500. We'll consider that success at this point.
        return http_result;
    }

    // We exceeded our retry limit. Return the last result received, which will be 500, 503, etc.
    return http_result;
}

Result<string>
PsiCash::BuildRequestParams(
        const std::string& method, const std::string& path,
        bool include_auth_tokens, const nlohmann::json& query_params, int attempt) const {
    json headers;
    headers["User-Agent"] = kPsiCashUserAgent;

    if (include_auth_tokens) {
        string s;
        for (const auto& at : user_data_->GetAuthTokens()) {
            if (!s.empty()) {
                s += ",";
            }
            s += at.second;
        }
        headers["X-PsiCash-Auth"] = s;
    }

    auto metadata = user_data_->GetRequestMetadata();
    metadata["attempt"] = attempt;
    headers["X-PsiCash-Metadata"] = metadata;

    json j = {
            {"scheme",   kAPIServerScheme},
            {"hostname", kAPIServerHostname},
            {"port",     kAPIServerPort},
            {"method",   method},
            {"path",     "/"s + kAPIServerVersion + path},
            {"query",    query_params},
            {"headers",  headers},
    };

    try {
        return j.dump();
    }
    catch (json::exception& e) {
        return MakeError(
                utils::Stringer("json dump failed: ", e.what(), "; id:", e.id).c_str());
    }
}

Result<PsiCash::NewExpiringPurchaseResponse> PsiCash::NewExpiringPurchase(
        const string& transaction_class,
        const string& distinguisher,
        const int64_t expected_price) {

    // TEMP
    auto earner = "569ee3e4784c39a3301285914f96c26746883f358c92fea16a8b2e41ad5be396";
    auto spender = "eb3f9a195447137c51bc475b7620eb008812cc47edfcb3f34d4347f5211ad0a8";
    auto indicator = "6058c5f924df70333271fe3899d543be7667edff62ddd5f39793c37809661a28";
    //auto tracker = "824e1ddb43aa1a957afc85ba236ad183";
    user_data_->SetAuthTokens({{"earner",    earner},
                               {"spender",   spender},
                               {"indicator", indicator}}, false);


    auto result = MakeHTTPRequestWithRetry(
            kMethodPOST,
            "/transaction",
            true,
            {
                    {"class",          transaction_class},
                    {"distinguisher",  distinguisher},
                    // Note the conversion from positive to negative: price to amount.
                    {"expectedAmount", -expected_price}
            }
    );
    if (!result) {
        return WrapError(result.error(), "MakeHTTPRequestWithRetry failed");
    }

    string transaction_id, authorization, transaction_type;
    datetime::DateTime server_expiry;

    // These status require the response body to be parsed
    if (result->status == kHTTPStatusOK ||
        result->status == kHTTPStatusTooManyRequests ||
        result->status == kHTTPStatusPaymentRequired ||
        result->status == kHTTPStatusConflict) {
        if (result->body.empty()) {
            return MakeError(
                    utils::Stringer("result has no body; status: ", result->status).c_str());
        }

        try {
            auto j = json::parse(result->body);

            // Many response fields are optional (depending on the presence of the indicator token)

            if (j["Balance"].is_number_integer()) {
                // We don't care about the return value of this right now
                (void) user_data_->SetBalance(j["Balance"].get<int64_t>());
            }

            if (j["TransactionID"].is_string()) {
                transaction_id = j["TransactionID"].get<string>();
            }

            if (j["Authorization"].is_string()) {
                authorization = j["Authorization"].get<string>();
            }

            if (j["TransactionResponse"]["Type"].is_string()) {
                transaction_type = j["TransactionResponse"]["Type"].get<string>();
            }

            if (j["TransactionResponse"]["Values"]["Expires"].is_string()) {
                string expiry_string = j["TransactionResponse"]["Values"]["Expires"].get<string>();
                if (!server_expiry.FromISO8601(expiry_string)) {
                    return MakeError(
                            ("failed to parse TransactionResponse.Values.Expires; got "s +
                             expiry_string).c_str());
                }
            }

            // Unused fields
            //auto transaction_amount = j.at("TransactionAmount").get<int64_t>();
        }
        catch (json::exception& e) {
            return MakeError(
                    utils::Stringer("json parse failed: ", e.what(), "; id:", e.id).c_str());
        }
    }

    if (result->status == kHTTPStatusOK) {
        if (transaction_type != "expiring-purchase") {
            return MakeError(
                    ("response contained incorrect TransactionResponse.Type; want 'expiring-purchase', got "s +
                     transaction_type).c_str());
        }
        if (transaction_id.empty()) {
            return MakeError("response did not provide valid TransactionID");
        }
        if (server_expiry.IsZero()) {
            return MakeError(
                    "response did not provide valid TransactionResponse.Values.Expires");
        }
        // Not checking authorization, as it doesn't apply to all expiring purchases

        Purchase purchase = {
                .id = transaction_id,
                .transaction_class = transaction_class,
                .distinguisher = distinguisher,
                .server_time_expiry = server_expiry,
                .authorization = authorization
        };

        if (auto err = user_data_->AddPurchase(purchase)) {
            return WrapError(err, "AddPurchase failed");
        }

        return PsiCash::NewExpiringPurchaseResponse{
                .status = PsiCashStatus_Success,
                .purchase = purchase
        };
    } else if (result->status == kHTTPStatusTooManyRequests) {
        return PsiCash::NewExpiringPurchaseResponse{
                .status = PsiCashStatus_ExistingTransaction
        };
    } else if (result->status == kHTTPStatusPaymentRequired) {
        return PsiCash::NewExpiringPurchaseResponse{
                .status = PsiCashStatus_InsufficientBalance
        };
    } else if (result->status == kHTTPStatusConflict) {
        return PsiCash::NewExpiringPurchaseResponse{
                .status = PsiCashStatus_TransactionAmountMismatch
        };
    } else if (result->status == kHTTPStatusNotFound) {
        return PsiCash::NewExpiringPurchaseResponse{
                .status = PsiCashStatus_TransactionTypeNotFound
        };
    } else if (result->status == kHTTPStatusUnauthorized) {
        return PsiCash::NewExpiringPurchaseResponse{
                .status = PsiCashStatus_InvalidTokens
        };
    } else if (result->status == kHTTPStatusInternalServerError) {
        return PsiCash::NewExpiringPurchaseResponse{
                .status = PsiCashStatus_ServerError
        };
    }

    return MakeError(
            utils::Stringer("request returned unexpected status code: ",
                            result->status).c_str());
}


// Enable JSON de/serializing of PurchasePrice.
// See https://github.com/nlohmann/json#basic-usage
bool operator==(const PurchasePrice& lhs, const PurchasePrice& rhs) {
    return lhs.transaction_class == rhs.transaction_class &&
           lhs.distinguisher == rhs.distinguisher &&
           lhs.price == rhs.price;
}

void to_json(json& j, const PurchasePrice& pp) {
    j = json{
            {"class",         pp.transaction_class},
            {"distinguisher", pp.distinguisher},
            {"price",         pp.price}};
}

void from_json(const json& j, PurchasePrice& pp) {
    pp.transaction_class = j.at("class").get<string>();
    pp.distinguisher = j.at("distinguisher").get<string>();
    pp.price = j.at("price").get<int64_t>();
}

// Enable JSON de/serializing of Purchase.
// See https://github.com/nlohmann/json#basic-usage
bool operator==(const Purchase& lhs, const Purchase& rhs) {
    return lhs.transaction_class == rhs.transaction_class &&
           lhs.distinguisher == rhs.distinguisher &&
           lhs.server_time_expiry == rhs.server_time_expiry &&
           //lhs.local_time_expiry == rhs.local_time_expiry && // Don't include the derived local time in the comparison
           lhs.authorization == rhs.authorization;
}

void to_json(json& j, const Purchase& p) {
    j = json{
            {"id",            p.id},
            {"class",         p.transaction_class},
            {"distinguisher", p.distinguisher}};

    if (p.authorization) {
        j["authorization"] = *p.authorization;
    } else {
        j["authorization"] = nullptr;
    }

    if (p.server_time_expiry) {
        j["serverTimeExpiry"] = *p.server_time_expiry;
    } else {
        j["serverTimeExpiry"] = nullptr;
    }

    if (p.local_time_expiry) {
        j["localTimeExpiry"] = *p.local_time_expiry;
    } else {
        j["localTimeExpiry"] = nullptr;
    }
}

void from_json(const json& j, Purchase& p) {
    p.id = j.at("id").get<string>();
    p.transaction_class = j.at("class").get<string>();
    p.distinguisher = j.at("distinguisher").get<string>();

    if (j.at("authorization").is_null()) {
        p.authorization = nullopt;
    } else {
        p.authorization = j.at("authorization").get<string>();
    }

    if (j.at("serverTimeExpiry").is_null()) {
        p.server_time_expiry = nullopt;
    } else {
        p.server_time_expiry = j.at("serverTimeExpiry").get<datetime::DateTime>();
    }

    if (j.at("localTimeExpiry").is_null()) {
        p.local_time_expiry = nullopt;
    } else {
        p.local_time_expiry = j.at("localTimeExpiry").get<datetime::DateTime>();
    }
}

} // namespace psicash
