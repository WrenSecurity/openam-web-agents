/**
 * The contents of this file are subject to the terms of the Common Development and
 * Distribution License (the License). You may not use this file except in compliance with the
 * License.
 *
 * You can obtain a copy of the License at legal/CDDLv1.0.txt. See the License for the
 * specific language governing permission and limitations under the License.
 *
 * When distributing Covered Software, include this CDDL Header Notice in each file and include
 * the License file at legal/CDDLv1.0.txt. If applicable, add the following below the CDDL
 * Header, with the fields enclosed by brackets [] replaced by your own identifying
 * information: "Portions copyright [year] [name of copyright owner]".
 *
 * Copyright 2014 - 2016 ForgeRock AS.
 */

#include "platform.h"
#include "version.h"
#ifdef __cplusplus
extern "C" {
#endif
#include "am.h"
#ifdef __cplusplus
}
#endif
#include <httpserv.h>

#define AM_MOD_SECTION                 L"system.webServer/OpenAmModule"
#define AM_MOD_SECTION_ENABLED         L"enabled"
#define AM_MOD_SECTION_CONFIGFILE      L"configFile"
#define AM_HTTP_STATUS_200             200
#define AM_HTTP_STATUS_302             302
#define AM_HTTP_STATUS_400             400
#define AM_HTTP_STATUS_403             403
#define AM_HTTP_STATUS_404             404
#define AM_HTTP_STATUS_500             500
#define AM_HTTP_STATUS_501             501

static IHttpServer *server = NULL;
static void *modctx = NULL;
static am_status_t set_user(am_request_t *rq, const char *user);
static am_status_t set_custom_response(am_request_t *rq, const char *text, const char *cont_type);
static am_status_t set_request_body(am_request_t *rq);
static am_status_t set_post_data_filename(am_request_t *rq, const char *value);

static DWORD hr_to_winerror(HRESULT hr) {
    if ((hr & 0xFFFF0000) == MAKE_HRESULT(SEVERITY_ERROR, FACILITY_WIN32, 0)) {
        return HRESULT_CODE(hr);
    }
    if (hr == S_OK) {
        return ERROR_SUCCESS;
    }
    return ERROR_CAN_NOT_COMPLETE;
}

static void *alloc_request(IHttpContext *r, size_t s) {
    void *ret = (void *) r->AllocateRequestMemory((DWORD) s);
    if (ret != NULL) {
        memset(ret, 0, s);
    }
    return ret;
}

static char *strndup_request(IHttpContext *r, const char *string, size_t size) {
    char *copy = (char *) alloc_request(r, size + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, string, size);
    return copy;
}

static char *utf8_encode(IHttpContext *r, const wchar_t *wstr, int *inout_len) {
    char *tmp = NULL;
    int in_len = inout_len != NULL ? *inout_len : -1; /* if no input length is set, assume NULL terminated string */

    int ret_len, out_len = wstr != NULL ?
            WideCharToMultiByte(CP_UTF8, 0, wstr, in_len, NULL, 0, NULL, NULL) : 0;
    if (out_len == 0) {
        return NULL;
    }

    tmp = r == NULL ? (char *) calloc(1, out_len + 1) : (char *) alloc_request(r, out_len + 1);
    if (tmp == NULL) {
        return NULL;
    }

    ret_len = WideCharToMultiByte(CP_UTF8, 0, wstr, in_len, tmp, (DWORD) out_len, NULL, NULL);
    if (inout_len) {
        *inout_len = ret_len;
    }
    return tmp;
}

static wchar_t *utf8_decode(IHttpContext *r, const char *str, size_t *outlen) {
    wchar_t *tmp = NULL;
    int ret_len, out_len = str != NULL ? MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0) : 0;
    if (outlen) *outlen = 0;
    if (out_len > 0) {
        tmp = (wchar_t *) alloc_request(r, sizeof (wchar_t) * out_len + 1);
        if (tmp != NULL) {
            memset(tmp, 0, sizeof (wchar_t) * out_len + 1);
            ret_len = MultiByteToWideChar(CP_UTF8, 0, str, -1, tmp, (DWORD) out_len);
            if (outlen) *outlen = ret_len - 1;
        }
        return tmp;
    }
    return NULL;
}

class OpenAMStoredConfig : public IHttpStoredContext{
    public :

    OpenAMStoredConfig() {
        enabled = FALSE;
        path = NULL;
        aconf = NULL;
    }

    ~OpenAMStoredConfig() {
        if (path != NULL) {
            delete [] path;
            path = NULL;
        }
        if (aconf != NULL) {
            am_config_free(&aconf);
            aconf = NULL;
        }
    }

    void CleanupStoredContext() {
        delete this;
    }

    BOOL IsEnabled() {
        return enabled;
    }

    char *GetPath(IHttpContext * r) {
        return utf8_encode(r, path, NULL);
    }

    am_config_t * GetBootConf() {
        return aconf;
    }

    static HRESULT GetConfig(IHttpContext *pContext, OpenAMStoredConfig **ppModuleConfig) {
        HRESULT hr = S_OK;
        OpenAMStoredConfig * pModuleConfig = NULL;
        IHttpModuleContextContainer * pMetadataContainer = NULL;
        IAppHostConfigException * pException = NULL;

        pMetadataContainer = pContext->GetMetadata()->GetModuleContextContainer();
        if (pMetadataContainer == NULL) {
            return E_UNEXPECTED;
        }

        pModuleConfig = (OpenAMStoredConfig *) pMetadataContainer->GetModuleContext(modctx);
        if (pModuleConfig != NULL) {
            *ppModuleConfig = pModuleConfig;
            return S_OK;
        }

        pModuleConfig = new OpenAMStoredConfig();
        if (pModuleConfig == NULL) {
            return E_OUTOFMEMORY;
        }

        hr = pModuleConfig->Initialize(pContext, &pException);
        if (FAILED(hr) || pException != NULL) {
            pModuleConfig->CleanupStoredContext();
            pModuleConfig = NULL;
            return E_UNEXPECTED;
        }

        hr = pMetadataContainer->SetModuleContext(pModuleConfig, modctx);
        if (FAILED(hr)) {
            pModuleConfig->CleanupStoredContext();
            pModuleConfig = NULL;
            if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_ASSIGNED)) {
                *ppModuleConfig = (OpenAMStoredConfig *) pMetadataContainer->GetModuleContext(modctx);
                return S_OK;
            }
        }

        *ppModuleConfig = pModuleConfig;
        return hr;
    }

    HRESULT Initialize(IHttpContext * pW3Context, IAppHostConfigException ** ppException) {
        HRESULT hr = S_OK;
        IAppHostAdminManager *pAdminManager = NULL;
        IAppHostElement *el = NULL;
        IAppHostPropertyException *excp = NULL;
        char *config = NULL;

        PCWSTR pszConfigPath = pW3Context->GetMetadata()->GetMetaPath();
        BSTR bstrUrlPath = SysAllocString(pszConfigPath);

        do {
            pAdminManager = server->GetAdminManager();
            if (pAdminManager == NULL) {
                hr = E_UNEXPECTED;
                break;
            }

            hr = pAdminManager->GetAdminSection(AM_MOD_SECTION, bstrUrlPath, &el);
            if (FAILED(hr)) {
                break;
            }
            if (el == NULL) {
                hr = E_UNEXPECTED;
                break;
            }

            hr = GetBooleanPropertyValue(el, AM_MOD_SECTION_ENABLED, &excp, &enabled);
            if (FAILED(hr)) {
                break;
            }
            if (excp != NULL) {
                ppException = (IAppHostConfigException**) & excp;
                hr = E_UNEXPECTED;
                break;
            }

            hr = GetStringPropertyValue(el, AM_MOD_SECTION_CONFIGFILE, &excp, &path);
            if (FAILED(hr)) {
                break;
            }
            if (excp != NULL) {
                ppException = (IAppHostConfigException**) & excp;
                hr = E_UNEXPECTED;
                break;
            }

            config = utf8_encode(NULL, path, NULL);
            if (config != NULL) {
                aconf = am_get_config_file(0, config);
                free(config);
            }


        } while (FALSE);

        SysFreeString(bstrUrlPath);
        return hr;
    }

private:

    HRESULT GetBooleanPropertyValue(IAppHostElement* pElement, WCHAR* pszPropertyName,
    IAppHostPropertyException** pException, BOOL * pBoolValue) {
        HRESULT hr = S_OK;
        IAppHostProperty *pProperty = NULL;
        VARIANT vPropertyValue;

        do {
            if (pElement == NULL || pszPropertyName == NULL ||
                    pException == NULL || pBoolValue == NULL) {
                hr = E_INVALIDARG;
                break;
            }

            hr = pElement->GetPropertyByName(pszPropertyName, &pProperty);
            if (FAILED(hr)) break;
            if (pProperty == NULL) {
                hr = E_UNEXPECTED;
                break;
            }

            VariantInit(&vPropertyValue);
            hr = pProperty->get_Value(&vPropertyValue);
            if (FAILED(hr)) break;

            *pException = NULL;
            hr = pProperty->get_Exception(pException);
            if (FAILED(hr)) break;
            if (*pException != NULL) {
                hr = E_UNEXPECTED;
                break;
            }

            *pBoolValue = (vPropertyValue.boolVal == VARIANT_TRUE) ? TRUE : FALSE;

        } while (FALSE);

        VariantClear(&vPropertyValue);
        if (pProperty != NULL) {
            pProperty->Release();
            pProperty = NULL;
        }
        return hr;
    }

    HRESULT GetStringPropertyValue(IAppHostElement* pElement, WCHAR* pszPropertyName,
    IAppHostPropertyException** pException, WCHAR ** ppszValue) {
        HRESULT hr = S_OK;
        IAppHostProperty *pProperty = NULL;
        DWORD dwLength;
        VARIANT vPropertyValue;

        do {

            if (pElement == NULL || pszPropertyName == NULL ||
                    pException == NULL || ppszValue == NULL) {
                hr = E_INVALIDARG;
                break;
            }

            *ppszValue = NULL;

            hr = pElement->GetPropertyByName(pszPropertyName, &pProperty);
            if (FAILED(hr)) break;
            if (pProperty == NULL) {
                hr = E_UNEXPECTED;
                break;
            }

            VariantInit(&vPropertyValue);
            hr = pProperty->get_Value(&vPropertyValue);
            if (FAILED(hr)) break;

            *pException = NULL;
            hr = pProperty->get_Exception(pException);
            if (FAILED(hr)) break;
            if (*pException != NULL) {
                hr = E_UNEXPECTED;
                break;
            }

            dwLength = SysStringLen(vPropertyValue.bstrVal);
            *ppszValue = new WCHAR[dwLength + 1];
            if (*ppszValue == NULL) {
                hr = E_OUTOFMEMORY;
                break;
            }

            wcsncpy(*ppszValue, vPropertyValue.bstrVal, dwLength);
            (*ppszValue)[dwLength] = L'\0';

        } while (FALSE);

        VariantClear(&vPropertyValue);
        if (pProperty != NULL) {
            pProperty->Release();
            pProperty = NULL;
        }
        return hr;
    }

    BOOL enabled;
    WCHAR *path;
    am_config_t *aconf;
};

class OpenAMHttpUser : public IHttpUser{
    public :

    virtual PCWSTR GetRemoteUserName(VOID) {
        return userName;
    }

    virtual PCWSTR GetUserName(VOID) {
        return userName;
    }

    virtual PCWSTR GetAuthenticationType(VOID) {
        return L"OpenAM";
    }

    virtual PCWSTR GetPassword(VOID) {
        return showPassword ? userPassword : L"";
    }

    virtual HANDLE GetImpersonationToken(VOID) {
        return hToken;
    }

    VOID SetImpersonationToken(HANDLE tkn) {
        hToken = tkn;
    }

    virtual HANDLE GetPrimaryToken(VOID) {
        return NULL;
    }

    virtual VOID ReferenceUser(VOID) {
        InterlockedIncrement(&m_refs);
    }

    virtual VOID DereferenceUser(VOID) {
        if (InterlockedDecrement(&m_refs) <= 0) {
            if (hToken) CloseHandle(hToken);
            delete this;
        }
    }

    virtual BOOL SupportsIsInRole(VOID) {
        return FALSE;
    }

    virtual HRESULT IsInRole(IN PCWSTR pszRoleName, OUT BOOL * pfInRole) {
        return E_NOTIMPL;
    }

    virtual PVOID GetUserVariable(IN PCSTR pszVariableName) {
        return NULL;
    }

    OpenAMHttpUser(PCWSTR usrn, PCWSTR usrp, PCWSTR usrpcrypted,
    BOOL showpass, BOOL dologon) : userName(usrn), userPassword(usrpcrypted),
    showPassword(showpass), status(FALSE), error(0) {
        HANDLE tkn = NULL;
        m_refs = 1;
        if (dologon) {
            if (usrn != NULL && usrp != NULL) {
                status = LogonUserW(usrn, NULL, usrp,
                        LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &tkn);
                error = GetLastError();
                if (status) {
                    SetImpersonationToken(tkn);
                }
            } else {
                error = ERROR_INVALID_DATA;
            }
        } else {
            SetImpersonationToken(tkn);
            status = TRUE;
        }
    }

    BOOL GetStatus() {
        return status;
    }

    DWORD GetError() {
        return error;
    }

private:

    LONG m_refs;
    PCWSTR userName;
    PCWSTR userPassword;
    HANDLE hToken;
    BOOL status;
    BOOL showPassword;
    DWORD error;

    virtual ~OpenAMHttpUser() {
    }
};

static const char *get_server_variable(IHttpContext *ctx,
        unsigned long instance_id, PCSTR var) {
    const char* thisfunc = "get_server_variable():";
    PCSTR val = NULL;
    DWORD size = 0;

    if (!ISVALID(var)) return NULL;

    AM_LOG_DEBUG(instance_id, "%s trying to fetch server variable %s", thisfunc, var);

    if (FAILED(ctx->GetServerVariable(var, &val, &size))) {
        AM_LOG_WARNING(instance_id,
                "%s server variable %s is not available in HttpContext (error: %d)",
                thisfunc, var, GetLastError());
    } else {
        AM_LOG_DEBUG(instance_id, "%s found variable %s with value [%s]",
                thisfunc, var, LOGEMPTY(val));
    }
    return val;
}

static const char *get_request_header(am_request_t *req, const char *name) {
    IHttpContext *ctx;
    if (req == NULL || (ctx = (IHttpContext *) req->ctx) == NULL || ISINVALID(name))
        return NULL;
    return get_server_variable(ctx, req->instance_id, name);
}

static am_status_t get_request_url(am_request_t *req) {
    static const char *thisfunc = "get_request_url():";
    IHttpContext *ctx;
    int cooked_url_sz;
    am_status_t status = AM_EINVAL;

    if (req == NULL) {
        return status;
    }

    ctx = (IHttpContext *) req->ctx;
    if (ctx == NULL) {
        return status;
    }

    if ((cooked_url_sz = ctx->GetRequest()->GetRawHttpRequest()->CookedUrl.FullUrlLength) == 0 ||
            ctx->GetRequest()->GetRawHttpRequest()->CookedUrl.pFullUrl == NULL) {
        return status;
    }

    /* convert the length of pFullUrl (FullUrlLength) in bytes to the number of wide characters */
    cooked_url_sz /= sizeof (wchar_t);

    char *cooked_url = utf8_encode(ctx, ctx->GetRequest()->GetRawHttpRequest()->CookedUrl.pFullUrl,
            &cooked_url_sz);
    if (cooked_url == NULL) {
        return AM_ENOMEM;
    }

    AM_LOG_DEBUG(req->instance_id, "%s pre-parsed request url: %s", thisfunc, cooked_url);
    req->orig_url = cooked_url;
    status = AM_SUCCESS;

    if (ctx->GetRequest()->GetRawHttpRequest()->pRawUrl != NULL &&
            ctx->GetRequest()->GetRawHttpRequest()->RawUrlLength > 0) {
        int j = 0;
        char *it = cooked_url;
        char *query = strchr(cooked_url, '?'); /* check if CookedUrl contains a query string */

        /* look up the third forward slash character and chop off everything else after it
         * (including the slash character itself) */
        while (*it != '\0') {
            if (*it++ == '/' && ++j == 3) {
                *--it = '\0';
                break;
            }
        }

        /* recreate the raw request url from 
         * HTTP_REQUEST::CookedUrl value of scheme://host:port combined 
         * with the HTTP_REQUEST::pRawUrl value (/path...) */
        char *raw_url = (char *) alloc_request(ctx, cooked_url_sz +
                ctx->GetRequest()->GetRawHttpRequest()->RawUrlLength + 1);
        if (raw_url == NULL) {
            return AM_ENOMEM;
        }

        strcpy(raw_url, cooked_url);
        strncat(raw_url, ctx->GetRequest()->GetRawHttpRequest()->pRawUrl,
                ctx->GetRequest()->GetRawHttpRequest()->RawUrlLength);
        /* if there was a query string in CookedUrl but pRawUrl didn't have it 
         * (request to the DefaultDocument with a query string for example)
         * append them here too
         */
        if (query != NULL && strchr(raw_url, '?') == NULL) {
            strcat(raw_url, query);
        }

        AM_LOG_DEBUG(req->instance_id, "%s reconstructed request url: %s", thisfunc, raw_url);
        req->orig_url = raw_url;
    }

    if (status == AM_SUCCESS) {
        char *path_info = (char *) get_server_variable(ctx, req->instance_id, "PATH_INFO");
        char *script_name = (char *) get_server_variable(ctx, req->instance_id, "SCRIPT_NAME");

        if (ISVALID(path_info) && ISVALID(script_name)) {
            /* remove the script name from path_info to get the real path info */
            const char *pos = strstr(path_info, script_name);
            size_t path_info_sz = strlen(path_info);
            if (pos == NULL) {
                AM_LOG_WARNING(req->instance_id, "%s script name %s not found in path info (%s). "
                        "Could not get the path info.", thisfunc, script_name, path_info);
                return status;
            }

            char *path_info_tmp = (char *) alloc_request(ctx, path_info_sz + 1);
            if (path_info_tmp != NULL) {
                strncpy(path_info_tmp, pos + strlen(script_name), path_info_sz);
                req->path_info = path_info_tmp;
                AM_LOG_DEBUG(req->instance_id, "%s reconstructed path info: %s", thisfunc,
                        path_info_tmp);
            }
        }
    }
    return status;
}

static am_status_t set_header_in_request(am_request_t *rq, const char *key, const char *value) {
    static const char *thisfunc = "set_header_in_request():";
    IHttpContext *r = (IHttpContext *) (rq != NULL ? rq->ctx : NULL);
    HRESULT hr;
    if (r == NULL || !ISVALID(key)) return AM_EINVAL;

    AM_LOG_DEBUG(rq->instance_id, "%s %s = %s", thisfunc, LOGEMPTY(key), LOGEMPTY(value));

    /* remove all instances of the header first */
    hr = r->GetRequest()->DeleteHeader(key);
    if (FAILED(hr)) {
        AM_LOG_WARNING(rq->instance_id, "%s failed to delete request header %s (%d)", thisfunc,
                LOGEMPTY(key), hr_to_winerror(hr));
    }
    if (ISVALID(value)) {
        size_t key_sz = strlen(key);
        size_t value_sz = strlen(value);
        char *key_data = (char *) alloc_request(r, key_sz + 1);
        char *value_data = (char *) alloc_request(r, value_sz + 1);
        if (key_data != NULL && value_data != NULL) {
            memcpy(key_data, key, key_sz);
            key_data[key_sz] = 0;
            memcpy(value_data, value, value_sz);
            value_data[value_sz] = 0;
            hr = r->GetRequest()->SetHeader(key_data, value_data, (USHORT) value_sz, TRUE);
            if (FAILED(hr)) {
                AM_LOG_WARNING(rq->instance_id, "%s failed to set request header %s value %s (%d)", thisfunc,
                        LOGEMPTY(key), LOGEMPTY(value), hr_to_winerror(hr));
                return AM_ERROR;
            }
        } else {
            return AM_ENOMEM;
        }
    }
    return AM_SUCCESS;
}

static am_status_t set_cookie(am_request_t *rq, const char *header) {
    static const char *thisfunc = "set_cookie():";
    am_status_t status = AM_SUCCESS;
    const char *current_cookies;
    char *cookie, *equals, *sep;
    size_t cookie_sz;
    HRESULT hr;
    IHttpContext *r = (IHttpContext *) (rq != NULL ? rq->ctx : NULL);
    if (r == NULL || ISINVALID(header)) return AM_EINVAL;

    /* add cookie in response headers */
    cookie_sz = strlen(header);
    cookie = strndup_request(r, header, cookie_sz);
    if (cookie == NULL) return AM_ENOMEM;

    hr = r->GetResponse()->SetHeader("Set-Cookie", cookie, (USHORT) cookie_sz, FALSE);
    if (FAILED(hr)) {
        AM_LOG_WARNING(rq->instance_id, "%s failed to set response header Set-Cookie value %s (%d)", thisfunc,
                LOGEMPTY(cookie), hr_to_winerror(hr));
        return AM_ERROR;
    }

    /* modify Cookie request header */
    equals = strchr(cookie, '=');
    sep = strchr(cookie, ';');
    current_cookies = get_server_variable(r, rq->instance_id, "HTTP_COOKIE");

    if (sep != NULL && equals != NULL && (sep - equals) > 1) {
        char *new_key = strndup_request(r, cookie, (equals - cookie) + 1); /* keep equals sign */
        char *new_value = strndup_request(r, cookie, sep - cookie);
        if (new_key == NULL || new_value == NULL) return AM_ENOMEM;
        if (ISINVALID(current_cookies)) {
            /* Cookie request header is not available yet - set it now */
            return set_header_in_request(rq, "Cookie", new_value);
        }
        if (strstr(current_cookies, new_key) == NULL) {
            /* append header value to the existing one */
            char *new_cookie = (char *) alloc_request(r, strlen(current_cookies) + strlen(new_value) + 2);
            if (new_cookie == NULL) return AM_ENOMEM;
            strcpy(new_cookie, current_cookies);
            strcat(new_cookie, ";");
            strcat(new_cookie, new_value);
            status = set_header_in_request(rq, "Cookie", new_cookie);
        }
    }
    return status;
}

static am_status_t add_header_in_response(am_request_t *rq, const char *key, const char *value) {
    static const char *thisfunc = "add_header_in_response():";
    am_status_t status = AM_ERROR;
    size_t key_sz, value_sz;
    char *key_data, *value_data;
    HRESULT hr;
    IHttpContext *r = (IHttpContext *) (rq != NULL ? rq->ctx : NULL);
    if (r == NULL || !ISVALID(key)) return AM_EINVAL;
    if (!ISVALID(value)) {
        /*value is empty, sdk is setting a cookie in response*/
        return set_cookie(rq, key);
    }
    key_sz = strlen(key);
    value_sz = strlen(value);
    key_data = (char *) alloc_request(r, key_sz + 1);
    value_data = (char *) alloc_request(r, value_sz + 1);
    if (key_data != NULL && value_data != NULL) {
        memcpy(key_data, key, key_sz);
        key_data[key_sz] = 0;
        memcpy(value_data, value, value_sz);
        value_data[value_sz] = 0;
        hr = r->GetResponse()->SetHeader(key_data, value_data, (USHORT) value_sz, FALSE);
        if (!FAILED(hr)) {
            status = AM_SUCCESS;
        } else {
            AM_LOG_WARNING(rq->instance_id, "%s failed to set response header %s value %s (%d)", thisfunc,
                    LOGEMPTY(key), LOGEMPTY(value), hr_to_winerror(hr));
        }
    } else {
        status = AM_ENOMEM;
    }
    return status;
}

static REQUEST_NOTIFICATION_STATUS am_status_value(IHttpContext *ctx, am_status_t v) {
    IHttpResponse *res = ctx->GetResponse();
    switch (v) {
        case AM_SUCCESS:
            return RQ_NOTIFICATION_CONTINUE;
        case AM_EAGAIN:
            return RQ_NOTIFICATION_PENDING;
        case AM_PDP_DONE:
        case AM_DONE:
            return RQ_NOTIFICATION_FINISH_REQUEST;
        case AM_NOT_HANDLING:
            return RQ_NOTIFICATION_CONTINUE;
        case AM_NOT_FOUND:
            res->SetStatus(AM_HTTP_STATUS_404, "Not Found");
            return RQ_NOTIFICATION_FINISH_REQUEST;
        case AM_REDIRECT:
            res->SetStatus(AM_HTTP_STATUS_302, "Found");
            return RQ_NOTIFICATION_FINISH_REQUEST;
        case AM_FORBIDDEN:
            res->SetStatus(AM_HTTP_STATUS_403, "Forbidden");
            return RQ_NOTIFICATION_FINISH_REQUEST;
        case AM_BAD_REQUEST:
            res->SetStatus(AM_HTTP_STATUS_400, "Bad Request");
            return RQ_NOTIFICATION_FINISH_REQUEST;
        case AM_ERROR:
            res->SetStatus(AM_HTTP_STATUS_500, "Internal Server Error");
            return RQ_NOTIFICATION_FINISH_REQUEST;
        case AM_NOT_IMPLEMENTED:
            res->SetStatus(AM_HTTP_STATUS_501, "Not Implemented");
            return RQ_NOTIFICATION_FINISH_REQUEST;
        default:
            res->SetStatus(AM_HTTP_STATUS_500, "Internal Server Error");
            return RQ_NOTIFICATION_FINISH_REQUEST;
    }
}

static am_status_t set_method(am_request_t *rq) {
    IHttpContext *r = (IHttpContext *) (rq != NULL ? rq->ctx : NULL);
    if (r == NULL) return AM_EINVAL;
    if (FAILED(r->GetRequest()->SetHttpMethod(am_method_num_to_str(rq->method)))) {
        return AM_ERROR;
    }
    return AM_SUCCESS;
}

static am_status_t get_request_body(am_request_t *rq) {
    static const char *thisfunc = "get_request_body():";
    IHttpContext *ctx = (IHttpContext *) (rq != NULL ? rq->ctx : NULL);
    IHttpRequest *r = ctx != NULL ? ctx->GetRequest() : NULL;
#define REQ_DATA_BUFF_SZ 1024
    DWORD read_bytes = 0, rc = REQ_DATA_BUFF_SZ, fpos, wrote, tmp_sz = 0;
    HRESULT hr;
    am_status_t status = AM_ERROR;
    char *out = NULL, *out_tmp, *tmp, *file_name = NULL;
    void *data;
    HANDLE fd = INVALID_HANDLE_VALUE;
    BOOL to_file = FALSE, tmp_writes = TRUE, write_status = FALSE;

    if (r == NULL)
        return AM_EINVAL;

#define TEMP_BUFFER_SZ  (REQ_DATA_BUFF_SZ * 2) 
    tmp = (char *) alloc_request(ctx, TEMP_BUFFER_SZ);
    if (tmp == NULL)
        return AM_ENOMEM;

    data = alloc_request(ctx, rc);
    if (data == NULL)
        return AM_ENOMEM;


    if (r->GetRemainingEntityBytes() == 0) {
        status = AM_SUCCESS;
    }

    while (r->GetRemainingEntityBytes() != 0) {
        hr = r->ReadEntityBody(data, rc, FALSE, &rc, NULL);
        if (FAILED(hr)) {
            if (ERROR_HANDLE_EOF != (hr & 0x0000FFFF)) {
                am_free(out);
                if (fd != INVALID_HANDLE_VALUE) {
                    CloseHandle(fd);
                }
                return AM_ERROR;
            }
            break;
        }

        if (tmp_writes && (tmp_sz + rc) <= TEMP_BUFFER_SZ) {
            /* stream initial data into the temp buffer */
            memcpy(tmp + tmp_sz, data, rc);
            tmp_sz += rc;
        }

        AM_LOG_DEBUG(rq->instance_id, "%s read: %ld, temp: %ld bytes",
                thisfunc, rc, tmp_sz);

        if (tmp_writes) {
            /* try to analyze temp buffer data - see if we can spot our key */
            if (tmp_sz > 5) {
                /* we've got enough data - check if that's 
                 * LARES POST or should it be stored into a file */
                tmp_writes = FALSE;
                to_file = memcmp(tmp, "LARES=", 6) != 0;
            } else {
                /* too little was read in */
                rc = REQ_DATA_BUFF_SZ;
                continue;
            }
        }

        AM_LOG_DEBUG(rq->instance_id, "%s storing into: %s, temp writes: %s",
                thisfunc, to_file ? "file" : "memory", !tmp_writes ? "done" : "working");

        if (!tmp_writes) {
            /* no more temp buffer screening - write data directly into a file (heap buffer) */
            if (to_file) {

                if (fd == INVALID_HANDLE_VALUE) {
                    char key[37];

                    if (ISINVALID(rq->conf->pdp_dir)) {
                        AM_LOG_ERROR(rq->instance_id, "%s invalid POST preservation configuration",
                                thisfunc);
                        return AM_EINVAL;
                    }

                    file_name = (char *) alloc_request(ctx, strlen(rq->conf->pdp_dir) + strlen(key) + 2);
                    if (file_name == NULL) {
                        return AM_ENOMEM;
                    }
                    uuid(key, sizeof (key));
                    sprintf(file_name, "%s/%s", rq->conf->pdp_dir, key);

                    fd = CreateFileA(file_name, FILE_GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (fd == INVALID_HANDLE_VALUE) {
                        AM_LOG_ERROR(rq->instance_id, "%s unable to create POST preservation file: %s (%d)",
                                thisfunc, file_name, GetLastError());
                        return AM_FILE_ERROR;
                    }
                }

                fpos = SetFilePointer(fd, 0, NULL, FILE_END);
                if (tmp_sz > 0) {
                    /* write down whatever we have stored in the temp buffer */
                    write_status = WriteFile(fd, tmp, tmp_sz, &wrote, NULL);
                    read_bytes += tmp_sz;
                    tmp_sz = 0;
                } else {
                    write_status = WriteFile(fd, data, rc, &wrote, NULL);
                    read_bytes += rc;
                }
                if (!write_status) {
                    AM_LOG_ERROR(rq->instance_id, "%s unable to write to POST preservation file: %s (%d)",
                            thisfunc, file_name, GetLastError());
                    CloseHandle(fd);
                    return AM_FILE_ERROR;
                }

            } else {

                /* process in-memory data */
                out_tmp = (char *) realloc(out, read_bytes + (tmp_sz > 0 ? tmp_sz : rc) + 1);
                if (out_tmp == NULL) {
                    am_free(out);
                    return AM_ENOMEM;
                }
                out = out_tmp;
                if (tmp_sz > 0) {
                    /* write down whatever we have stored in the temp buffer */
                    memcpy(out + read_bytes, tmp, tmp_sz);
                    read_bytes += tmp_sz;
                    tmp_sz = 0;
                } else {
                    memcpy(out + read_bytes, data, rc);
                    read_bytes += rc;
                }
                out[read_bytes] = 0;
            }
        }

        status = AM_SUCCESS;
        rc = REQ_DATA_BUFF_SZ;
    }

    rq->post_data = out;
    rq->post_data_fn = ISVALID(file_name) ? strdup(file_name) : NULL;
    rq->post_data_sz = read_bytes;

    if (fd != INVALID_HANDLE_VALUE) {
        CloseHandle(fd);
    }

    if (status == AM_SUCCESS) {
        AM_LOG_DEBUG(rq->instance_id, "%s processed %d bytes\n%s", thisfunc,
                read_bytes, ISVALID(out) ? out : LOGEMPTY(file_name));
        r->DeleteHeader("CONTENT_LENGTH");
    }
    return status;
}

static am_status_t des_decrypt(const char *encrypted, const char *keys, char **clear) {
    am_status_t status = AM_ERROR;
    HCRYPTPROV hCryptProv;
    HCRYPTKEY hKey;
    BYTE IV[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    BYTE bKey[20], *data = NULL, *key = NULL;
    DWORD keyLen = 8, dataLen = 0;
    int i;
    BLOBHEADER keyHeader;
    size_t enc_sz = strlen(encrypted);
    size_t key_sz = strlen(keys);

    data = (BYTE *) base64_decode(encrypted, &enc_sz);
    dataLen = (DWORD) enc_sz;
    key = (BYTE *) base64_decode(keys, &key_sz);
    if (dataLen > 0 && key_sz > 0) {
        keyHeader.bType = PLAINTEXTKEYBLOB;
        keyHeader.bVersion = CUR_BLOB_VERSION;
        keyHeader.reserved = 0;
        keyHeader.aiKeyAlg = CALG_DES;
        for (i = 0; i<sizeof (keyHeader); i++) {
            bKey[i] = *((BYTE*) & keyHeader + i);
        }
        for (i = 0; i<sizeof (keyLen); i++) {
            bKey[i + sizeof (keyHeader)] = *((BYTE*) & keyLen + i);
        }
        for (i = 0; i < 8; i++) {
            bKey[i + sizeof (keyHeader) + sizeof (keyLen)] = key[i];
        }
        if (CryptAcquireContext(&hCryptProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
            if (CryptImportKey(hCryptProv, (BYTE*) & bKey, sizeof (keyHeader) + sizeof (DWORD) + 8, 0, 0, &hKey)) {
                DWORD desMode = CRYPT_MODE_ECB;
                CryptSetKeyParam(hKey, KP_MODE, (BYTE*) & desMode, 0);
                DWORD padding = ZERO_PADDING;
                CryptSetKeyParam(hKey, KP_PADDING, (BYTE*) & padding, 0);
                CryptSetKeyParam(hKey, KP_IV, &IV[0], 0);
                if (CryptDecrypt(hKey, 0, FALSE, 0, data, &dataLen)) {
                    *clear = (char *) calloc(1, (size_t) dataLen + 1);
                    memcpy(*clear, data, (size_t) dataLen);
                    (*clear)[dataLen] = 0;
                    status = AM_SUCCESS;
                }
            }
        }
        CryptDestroyKey(hKey);
        CryptReleaseContext(hCryptProv, 0);
    }
    am_free(data);
    am_free(key);
    return status;
}

static void send_custom_data(IHttpResponse *res, const char *payload, int payload_sz,
        const char *cont_type, int status) {
    HTTP_DATA_CHUNK dc;
    DWORD sent;
    char payload_sz_text[64];
    BOOL cmpl = FALSE;
    struct http_status *ht = get_http_status(status);

    if (res == NULL) return;
    res->SetStatus(ht->code, ht->reason, 0, S_OK);
    res->SetHeader("Content-Type", cont_type,
            (USHORT) strlen(cont_type), TRUE);
    snprintf(payload_sz_text, sizeof (payload_sz_text), "%d", payload_sz);
    res->SetHeader("Content-Length", payload_sz_text, (USHORT) strlen(payload_sz_text), TRUE);

    dc.DataChunkType = HttpDataChunkFromMemory;
    dc.FromMemory.pBuffer = (PVOID) NOTNULL(payload);
    dc.FromMemory.BufferLength = (USHORT) payload_sz;
    res->WriteEntityChunks(&dc, 1, FALSE, TRUE, &sent);
    res->Flush(FALSE, FALSE, &sent, &cmpl);
}

class OpenAMHttpModule : public CHttpModule{
    public :

    OpenAMHttpModule(int status) {
        doLogOn = FALSE;
        showPassword = FALSE;
        userName = NULL;
        userPassword = NULL;
        userPasswordCrypted = NULL;
        userPasswordSize = 0;
        userPasswordCryptedSize = 0;
        clonedContext = NULL;
        pdp_file = INVALID_HANDLE_VALUE;
        pdp_file_data = NULL;
        pdp_file_map = NULL;
        pdp_file_name = NULL;
        init_status = status;
    }

    REQUEST_NOTIFICATION_STATUS OnAsyncCompletion(IHttpContext * ctx, DWORD dwNotification,
    BOOL fPostNotification, IHttpEventProvider * prov, IHttpCompletionInfo * pCompletionInfo) {
        if (clonedContext != NULL) {
            clonedContext->ReleaseClonedContext();
            clonedContext = NULL;
        }
        return RQ_NOTIFICATION_CONTINUE;
    }

    REQUEST_NOTIFICATION_STATUS OnBeginRequest(IHttpContext *ctx,
    IHttpEventProvider * prov) {
        static const char *thisfunc = "OpenAMHttpModule():";
        REQUEST_NOTIFICATION_STATUS status = RQ_NOTIFICATION_CONTINUE;
        IHttpRequest *req = ctx->GetRequest();
        IHttpResponse *res = ctx->GetResponse();
        IHttpSite *site = ctx->GetSite();
        HRESULT hr = S_OK;
        int rv;
        am_request_t d;
        const am_config_t *boot = NULL;
        am_config_t *rq_conf = NULL;
        OpenAMStoredConfig *conf = NULL;
        char ip[INET6_ADDRSTRLEN];
        USHORT status_code;

        /* agent module is not enabled for this 
         * server/site - we are not handling this request
         **/
        hr = OpenAMStoredConfig::GetConfig(ctx, &conf);
        if (FAILED(hr) || conf->IsEnabled() == FALSE) {
            return RQ_NOTIFICATION_CONTINUE;
        }
        
        if (init_status != AM_SUCCESS) {
            AM_LOG_ERROR(0, "%s agent init for site %d failed (error: %d)",
                    thisfunc, site->GetSiteId(), init_status);
            res->SetStatus(AM_HTTP_STATUS_500, "Internal Server Error");
            return RQ_NOTIFICATION_FINISH_REQUEST;
        }

        boot = conf->GetBootConf();
        if (boot != NULL) {
            /* register and update instance logger configuration (for already registered
             * instances - update logging level only)
             */
            am_log_register_instance(site->GetSiteId(), boot->debug_file, boot->debug_level, boot->debug,
                    boot->audit_file, boot->audit_level, boot->audit, conf->GetPath(ctx));
        } else {
            res->SetStatus(AM_HTTP_STATUS_500, "Internal Server Error");
            return RQ_NOTIFICATION_FINISH_REQUEST;
        }

        AM_LOG_DEBUG(site->GetSiteId(), "%s begin", thisfunc);

        res->DisableKernelCache(9);

        rv = am_get_agent_config(site->GetSiteId(), conf->GetPath(ctx), &rq_conf);
        if (rq_conf == NULL || rv != AM_SUCCESS) {
            AM_LOG_ERROR(site->GetSiteId(), "%s failed to get agent configuration instance, error: %s",
                    thisfunc, am_strerror(rv));
            res->SetStatus(AM_HTTP_STATUS_403, "Forbidden");
            return RQ_NOTIFICATION_FINISH_REQUEST;
        }

        /* set up request processor data structure */
        memset(&d, 0, sizeof (am_request_t));
        d.conf = rq_conf;
        d.status = AM_ERROR;
        d.instance_id = site->GetSiteId();
        d.ctx = ctx;
        d.ctx_class = this;
        d.method = am_method_str_to_num(req->GetHttpMethod());
        d.content_type = get_server_variable(ctx, d.instance_id, "CONTENT_TYPE");
        d.cookies = get_server_variable(ctx, d.instance_id, "HTTP_COOKIE");

        if (ISVALID(d.conf->client_ip_header)) {
            d.client_ip = (char *) get_server_variable(ctx, d.instance_id,
                    d.conf->client_ip_header);
        }
        if (!ISVALID(d.client_ip)) {
            unsigned long s = sizeof (ip);
            PSOCKADDR sa = req->GetRemoteAddress();
            if (sa != NULL) {
                memset(&ip[0], 0, sizeof (ip));
                if (sa->sa_family == AF_INET) {
                    struct sockaddr_in *ipv4 = reinterpret_cast<struct sockaddr_in *>(sa);
                    if (WSAAddressToStringA((LPSOCKADDR) ipv4, sizeof (*ipv4), NULL, ip, &s) == 0) {
                        char *b = strchr(ip, ':');
                        if (b != NULL) *b = 0;
                        d.client_ip = ip;
                    }
                } else {
                    struct sockaddr_in6 *ipv6 = reinterpret_cast<struct sockaddr_in6 *>(sa);
                    if (WSAAddressToStringA((LPSOCKADDR) ipv6, sizeof (*ipv6), NULL, ip, &s) == 0) {
                        char *b;
                        if (ip[0] == '[') {
                            memmove(ip, ip + 1, s - 2);
                        }
                        b = strchr(ip, ']');
                        if (b != NULL) *b = 0;
                        d.client_ip = ip;
                    }
                }
            }
        }
        if (ISVALID(d.conf->client_hostname_header)) {
            d.client_host = (char *) get_server_variable(ctx, d.instance_id,
                    d.conf->client_hostname_header);
        }

        d.am_get_request_url_f = get_request_url;
        d.am_get_post_data_f = get_request_body;
        d.am_set_post_data_f = set_request_body;
        d.am_set_user_f = set_user;
        d.am_set_header_in_request_f = set_header_in_request;
        d.am_add_header_in_response_f = add_header_in_response;
        d.am_set_cookie_f = set_cookie;
        d.am_set_custom_response_f = set_custom_response;
        d.am_set_post_data_filename_f = set_post_data_filename;
        d.am_get_request_header_f = get_request_header;

        am_process_request(&d);

        status = am_status_value(ctx, d.status);

        res->GetStatus(&status_code, NULL, NULL, NULL, &hr, NULL, NULL, NULL);

        /* json handler for the rest of the unsuccessful exit statuses not processed by set_custom_response */
        if (d.is_json_url && status_code > AM_HTTP_STATUS_302) {
            char *payload = NULL;
            int payload_sz = am_asprintf(&payload, AM_JSON_TEMPLATE_DATA,
                    am_strerror(d.status), "\"\"", status_code);
            send_custom_data(res, payload, payload_sz, "application/json", AM_HTTP_STATUS_200);
            status = RQ_NOTIFICATION_FINISH_REQUEST;
            am_free(payload);
        }

        AM_LOG_DEBUG(site->GetSiteId(), "%s exit status: %s (%d), HTTP status: %d", thisfunc, am_strerror(d.status), d.status, status_code);

        am_config_free(&d.conf);
        am_request_free(&d);

        return status;
    }

    REQUEST_NOTIFICATION_STATUS OnAuthenticateRequest(IHttpContext *ctx,
    IAuthenticationProvider * prov) {
        IHttpUser *currentUser = ctx->GetUser();
        IHttpResponse *res = ctx->GetResponse();
        IHttpSite *site = ctx->GetSite();
        if (currentUser == NULL) {
            if (userName != NULL) {
                PCWSTR user = utf8_decode(ctx, userName, NULL);
                OpenAMHttpUser *httpUser = new OpenAMHttpUser(user, userPassword,
                        userPasswordCrypted, showPassword, doLogOn);
                if (httpUser == NULL || !httpUser->GetStatus()) {
                    AM_LOG_ERROR(site->GetSiteId(), "OpenAMHttpModule(): failed (invalid Windows/AD user credentials). "
                            "Responding with HTTP403 error (%d)", httpUser == NULL ? ERROR_OUTOFMEMORY : httpUser->GetError());
                    res->SetStatus(AM_HTTP_STATUS_403, "Forbidden");
                    return RQ_NOTIFICATION_FINISH_REQUEST;
                } else {
                    AM_LOG_DEBUG(site->GetSiteId(), "OpenAMHttpModule(): context user set to \"%s\"", userName);
                }
                prov->SetUser(httpUser);
            } else {
                AM_LOG_DEBUG(site->GetSiteId(), "OpenAMHttpModule(): context user is not available");
            }
        }
        return RQ_NOTIFICATION_CONTINUE;
    }

    REQUEST_NOTIFICATION_STATUS OnEndRequest(IHttpContext *ctx,
    IHttpEventProvider * pProvider) {
        if (userPassword != NULL && userPasswordSize > 0) {
            SecureZeroMemory((PVOID) userPassword, userPasswordSize);
        }
        if (userPasswordCrypted != NULL && userPasswordCryptedSize > 0) {
            SecureZeroMemory((PVOID) userPasswordCrypted, userPasswordCryptedSize);
        }
        if (pdp_file_data) {
            UnmapViewOfFile(pdp_file_data);
        }
        if (pdp_file_map) {
            CloseHandle(pdp_file_map);
        }
        if (pdp_file != INVALID_HANDLE_VALUE) {
            CloseHandle(pdp_file);
        }
        if (pdp_file_name) {
            DeleteFileA(pdp_file_name);
        }
        return RQ_NOTIFICATION_CONTINUE;
    }

    char *userName;
    wchar_t *userPassword;
    DWORD userPasswordSize;
    wchar_t *userPasswordCrypted;
    DWORD userPasswordCryptedSize;
    BOOL showPassword;
    BOOL doLogOn;
    IHttpContext *clonedContext;
    HANDLE pdp_file;
    HANDLE pdp_file_map;
    LPVOID pdp_file_data;
    char *pdp_file_name;
    int init_status;
};

static am_status_t set_request_body(am_request_t *rq) {
    static const char *thisfunc = "set_request_body():";
    IHttpContext *r = (IHttpContext *) (rq != NULL ? rq->ctx : NULL);
    am_status_t status = AM_SUCCESS;
    HRESULT hr = S_OK;
    OpenAMHttpModule *m = rq != NULL && rq->ctx_class != NULL ?
            static_cast<OpenAMHttpModule *>(rq->ctx_class) : NULL;

    if (r == NULL)
        return AM_EINVAL;

    if (ISVALID(rq->post_data_fn) && rq->post_data_sz > 0) {

#define CONTENT_LENGTH_CSZ 128
        char *value = (char *) alloc_request(r, CONTENT_LENGTH_CSZ + 1);
        snprintf(value, CONTENT_LENGTH_CSZ, "%ld", rq->post_data_sz);

        m->pdp_file = CreateFileA(rq->post_data_fn, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (m->pdp_file == INVALID_HANDLE_VALUE) {
            AM_LOG_ERROR(rq->instance_id, "%s unable to open POST preservation file: %s (%d)",
                    thisfunc, rq->post_data_fn, GetLastError());
            DeleteFileA(rq->post_data_fn);
            return AM_FILE_ERROR;
        }
        /* when pdp_file_name is set (default), remove temp file in OnEndRequest */
        m->pdp_file_name = strndup_request(r, rq->post_data_fn, strlen(rq->post_data_fn));

        m->pdp_file_map = CreateFileMappingA(m->pdp_file, NULL, PAGE_READWRITE, 0, (DWORD) rq->post_data_sz, NULL);
        if (!m->pdp_file_map) {
            CloseHandle(m->pdp_file);
            AM_LOG_ERROR(rq->instance_id, "%s unable to map POST preservation file: %s (%d)",
                    thisfunc, rq->post_data_fn, GetLastError());
            return AM_FILE_ERROR;
        }

        m->pdp_file_data = MapViewOfFile(m->pdp_file_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!m->pdp_file_data) {
            CloseHandle(m->pdp_file_map);
            CloseHandle(m->pdp_file);
            AM_LOG_ERROR(rq->instance_id, "%s unable to map-view POST preservation file: %s (%d)",
                    thisfunc, rq->post_data_fn, GetLastError());
            return AM_FILE_ERROR;
        }

        hr = r->GetRequest()->InsertEntityBody(m->pdp_file_data, (DWORD) rq->post_data_sz);
        if (hr != S_OK) {
            status = AM_ERROR;
        } else {
            r->GetRequest()->SetHeader("CONTENT_LENGTH", value, (USHORT) strlen(value), TRUE);
        }
    }
    if (status != AM_SUCCESS) {
        AM_LOG_WARNING(rq->instance_id, "%s status %s (%d)", thisfunc,
                am_strerror(status), hr_to_winerror(hr));
    }
    return status;
}

static am_status_t set_post_data_filename(am_request_t *rq, const char *value) {
    IHttpContext *r = (IHttpContext *) (rq != NULL ? rq->ctx : NULL);
    OpenAMHttpModule *m = rq != NULL && rq->ctx_class != NULL ?
            static_cast<OpenAMHttpModule *>(rq->ctx_class) : NULL;
    if (m == NULL || r == NULL)
        return AM_EINVAL;
    m->pdp_file_name = ISVALID(value) ? strndup_request(r, value, strlen(value)) : NULL;
    return AM_SUCCESS;
}

static am_status_t set_user(am_request_t *rq, const char *user) {
    static const char *thisfunc = "set_user():";
    IHttpContext *r = (IHttpContext *) (rq != NULL ? rq->ctx : NULL);
    OpenAMHttpModule *m = rq != NULL && rq->ctx_class != NULL ?
            static_cast<OpenAMHttpModule *>(rq->ctx_class) : NULL;
    if (m == NULL || r == NULL || ISINVALID(user)) return AM_EINVAL;

    m->userName = (char *) alloc_request(r, strlen(user) + 1);
    if (m->userName != NULL) {
        strcpy(m->userName, user);
        AM_LOG_DEBUG(rq->instance_id, "%s creating context user \"%s\"", thisfunc, user);
    } else {
        AM_LOG_ERROR(rq->instance_id, "%s memory allocation error", thisfunc);
        return AM_ENOMEM;
    }

    if (ISVALID(rq->user_password) && ISVALID(rq->conf->password_replay_key)) {
        char *user_passwd = NULL;
        AM_LOG_DEBUG(rq->instance_id, "%s setting up user \"%s\" password", thisfunc, user);
        if (des_decrypt(rq->user_password, rq->conf->password_replay_key, &user_passwd) == AM_SUCCESS) {
            m->userPassword = utf8_decode(r, user_passwd, (size_t *) & m->userPasswordSize);
        } else {
            AM_LOG_WARNING(rq->instance_id, "%s user \"%s\" password decryption failed", thisfunc, user);
        }
        am_free(user_passwd);
        m->userPasswordCrypted = utf8_decode(r, rq->user_password, (size_t *) & m->userPasswordCryptedSize);
    }

    m->doLogOn = rq->conf->logon_user_enable ? TRUE : FALSE;
    if (m->doLogOn && ISVALID(m->userPassword)) {
        AM_LOG_DEBUG(rq->instance_id, "%s will try to logon user \"%s\"", thisfunc, user);
    }

    m->showPassword = rq->conf->password_header_enable ? TRUE : FALSE;
    if (m->showPassword && ISVALID(m->userPasswordCrypted)) {
        AM_LOG_DEBUG(rq->instance_id,
                "%s will try to setup encrypted password for user \"%s\" (AUTH_PASSWORD)", thisfunc, user);
    }
    return AM_SUCCESS;
}

static am_status_t set_custom_response(am_request_t *rq, const char *text, const char *cont_type) {
    static const char *thisfunc = "set_custom_response():";
    HRESULT hr;
    IHttpContext *r = (IHttpContext *) (rq != NULL ? rq->ctx : NULL);
    OpenAMHttpModule *m = rq != NULL && rq->ctx_class != NULL ?
            static_cast<OpenAMHttpModule *>(rq->ctx_class) : NULL;
    am_status_t status = AM_ERROR;
    if (r == NULL) {
        return AM_EINVAL;
    }

    status = rq->is_json_url ? AM_JSON_RESPONSE : rq->status;

    switch (status) {
        case AM_JSON_RESPONSE:
        {
            char *payload = NULL;
            int payload_sz = 0;
            switch (rq->status) {
                case AM_PDP_DONE:
                {
                    size_t data_sz = rq->post_data_sz;
                    char *temp = NULL;
                    if (m->pdp_file_data) {
                        temp = base64_encode(m->pdp_file_data, &data_sz);
                    }
                    payload_sz = am_asprintf(&payload, AM_JSON_TEMPLATE_LOCATION_DATA,
                            am_strerror(rq->status), rq->post_data_url, cont_type,
                            NOTNULL(temp), AM_HTTP_STATUS_200);
                    am_free(temp);
                    break;
                }
                case AM_REDIRECT:
                case AM_INTERNAL_REDIRECT:
                    payload_sz = am_asprintf(&payload, AM_JSON_TEMPLATE_LOCATION,
                            am_strerror(rq->status), text, AM_HTTP_STATUS_302);
                    if (is_http_status(rq->conf->json_url_response_code)) {
                        send_custom_data(r->GetResponse(), payload, payload_sz,
                                "application/json", rq->conf->json_url_response_code);
                    } else {
                        if (rq->conf->json_url_response_code != 0) {
                            AM_LOG_WARNING(rq->instance_id, "%s response status code %d is not valid, sending HTTP_FORBIDDEN",
                                    thisfunc, rq->conf->json_url_response_code);
                        }
                        send_custom_data(r->GetResponse(), payload, payload_sz,
                                "application/json", AM_HTTP_STATUS_403);
                    }
                    rq->status = AM_DONE;
                    am_free(payload);
                    return AM_SUCCESS;
                default:
                {
                    char *temp = am_json_escape(text, NULL);
                    payload_sz = am_asprintf(&payload, AM_JSON_TEMPLATE_DATA,
                            am_strerror(rq->status), ISVALID(temp) ? temp : "\"\"", AM_HTTP_STATUS_200);
                    am_free(temp);
                    break;
                }
            }
            send_custom_data(r->GetResponse(), payload, payload_sz, "application/json", AM_HTTP_STATUS_200);
            rq->status = AM_DONE;
            am_free(payload);
            break;
        }
        case AM_INTERNAL_REDIRECT:
        case AM_REDIRECT:
        {
            hr = r->GetResponse()->Redirect(text, TRUE, FALSE);
            if (FAILED(hr)) {
                AM_LOG_ERROR(rq->instance_id, "set_custom_response(): failed to issue a redirect to %s (%d)",
                        text, hr_to_winerror(hr));
                rq->status = AM_ERROR;
                break;
            }
            rq->status = AM_DONE;
            break;
        }
        case AM_PDP_DONE:
        {
            IHttpContext *sr = NULL;
            BOOL completion_expected;

            /* special handler for x-www-form-urlencoded POST data */
            if (_stricmp(cont_type, "application/x-www-form-urlencoded") == 0) {
                char *pair, *a, *eq, *last = NULL;
                char *form = NULL;
                int form_sz;

                form_sz = am_asprintf(&form, "<html><head></head><body onload=\"document.postform.submit()\">"
                        "<form name=\"postform\" method=\"POST\" action=\"%s\">", rq->post_data_url);
                if (form == NULL) {
                    AM_LOG_ERROR(rq->instance_id, "set_custom_response(): memory allocation error");
                    rq->status = AM_ERROR;
                    break;
                }

                if (m->pdp_file_data) {
                    a = (char *) alloc_request(r, rq->post_data_sz + 1);
                    if (a == NULL) {
                        AM_LOG_ERROR(rq->instance_id, "set_custom_response(): memory allocation error");
                        rq->status = AM_ERROR;
                        break;
                    }
                    memcpy(a, m->pdp_file_data, rq->post_data_sz);
                    for (pair = strtok_s(a, "&", &last); pair;
                            pair = strtok_s(NULL, "&", &last)) {
                        UrlUnescapeA(pair, NULL, NULL, URL_UNESCAPE_INPLACE);
                        eq = strchr(pair, '=');
                        if (eq) {
                            *eq++ = 0;
                            form_sz = am_asprintf(&form,
                                    "%s<input type=\"hidden\" name=\"%s\" value=\"%s\"/>",
                                    form, pair, eq);
                        } else {
                            form_sz = am_asprintf(&form,
                                    "%s<input type=\"hidden\" name=\"%s\" value=\"\"/>",
                                    form, pair);
                        }
                    }
                }
                form_sz = am_asprintf(&form, "%s</form></body></html>", form);
                if (form == NULL) {
                    AM_LOG_ERROR(rq->instance_id, "set_custom_response(): memory allocation error");
                    rq->status = AM_ERROR;
                    break;
                }
                send_custom_data(r->GetResponse(), form, form_sz, "text/html", AM_HTTP_STATUS_200);
                rq->status = AM_DONE;
                am_free(form);
                break;
            }

            /* all other content types are replied in a sub-request */
            hr = r->GetRequest()->SetHeader("Content-Type", cont_type,
                    (USHORT) strlen(cont_type), TRUE);
            if (FAILED(hr)) {
                AM_LOG_ERROR(rq->instance_id, "set_custom_response(): SetHeader failed for %s (%d)",
                        LOGEMPTY(rq->post_data_url), hr_to_winerror(hr));
                rq->status = AM_ERROR;
                break;
            }
            hr = r->GetRequest()->SetHttpMethod(am_method_num_to_str(rq->method));
            if (FAILED(hr)) {
                AM_LOG_ERROR(rq->instance_id, "set_custom_response(): SetHttpMethod failed for %s (%d)",
                        LOGEMPTY(rq->post_data_url), hr_to_winerror(hr));
                rq->status = AM_ERROR;
                break;
            }
            hr = r->GetRequest()->SetUrl(rq->post_data_url,
                    (DWORD) strlen(rq->post_data_url), TRUE);
            if (FAILED(hr)) {
                AM_LOG_ERROR(rq->instance_id, "set_custom_response(): SetUrl failed for %s (%d)",
                        LOGEMPTY(rq->post_data_url), hr_to_winerror(hr));
                rq->status = AM_ERROR;
                break;
            }

            hr = r->CloneContext(CLONE_FLAG_BASICS | CLONE_FLAG_HEADERS | CLONE_FLAG_ENTITY, &sr);
            if (FAILED(hr)) {
                AM_LOG_ERROR(rq->instance_id, "set_custom_response(): CloneContext failed for %s (%d)",
                        LOGEMPTY(rq->post_data_url), hr_to_winerror(hr));
                rq->status = AM_ERROR;
                break;
            }

            hr = sr->GetRequest()->SetHttpMethod(am_method_num_to_str(rq->method));
            if (FAILED(hr)) {
                AM_LOG_ERROR(rq->instance_id, "set_custom_response(): SetHttpMethod failed for %s (%d)",
                        LOGEMPTY(rq->post_data_url), hr_to_winerror(hr));
                rq->status = AM_ERROR;
                break;
            }
            hr = sr->GetRequest()->SetUrl(rq->post_data_url,
                    (DWORD) strlen(rq->post_data_url), TRUE);
            if (FAILED(hr)) {
                AM_LOG_ERROR(rq->instance_id, "set_custom_response(): SetUrl failed for %s (%d)",
                        LOGEMPTY(rq->post_data_url), hr_to_winerror(hr));
                rq->status = AM_ERROR;
                break;
            }
            hr = r->ExecuteRequest(FALSE, sr, EXECUTE_FLAG_BUFFER_RESPONSE,
                    NULL, &completion_expected);
            if (FAILED(hr)) {
                AM_LOG_ERROR(rq->instance_id, "set_custom_response(): ExecuteRequest failed for %s (%d)",
                        LOGEMPTY(rq->post_data_url), hr_to_winerror(hr));
                rq->status = AM_ERROR;
                break;
            }

            sr->ReleaseClonedContext();
            rq->status = AM_SUCCESS;
            break;
        }
        default:
        {
            HTTP_DATA_CHUNK dc;
            DWORD sent;
            char tls[64];
            size_t tl = strlen(text);
            snprintf(tls, sizeof (tls), "%d", tl);
            if (rq->status == AM_SUCCESS || rq->status == AM_DONE) {
                hr = r->GetResponse()->SetStatus(AM_HTTP_STATUS_200, "OK", 0, S_OK);
            } else {
                r->GetResponse()->Clear();
                am_status_value(r, rq->status);
            }
            if (ISVALID(cont_type)) {
                hr = r->GetResponse()->SetHeader("Content-Type", cont_type,
                        (USHORT) strlen(cont_type), TRUE);
            }
            hr = r->GetResponse()->SetHeader("Content-Length", tls, (USHORT) strlen(tls), TRUE);
            dc.DataChunkType = HttpDataChunkFromMemory;
            dc.FromMemory.pBuffer = (PVOID) text;
            dc.FromMemory.BufferLength = (USHORT) tl;
            hr = r->GetResponse()->WriteEntityChunks(&dc, 1, FALSE, TRUE, &sent);
            rq->status = AM_DONE;
            break;
        }
    }
    AM_LOG_DEBUG(rq->instance_id, "set_custom_response(): status: %s (exit: %s)",
            am_strerror(status), am_strerror(rq->status));
    return AM_SUCCESS;
}

class OpenAMHttpModuleFactory : public IHttpModuleFactory{
    public :

    OpenAMHttpModuleFactory() {
        status = am_init_worker(AM_DEFAULT_AGENT_ID);
    }

    virtual HRESULT GetHttpModule(CHttpModule **mm, IModuleAllocator * ma) {
        OpenAMHttpModule *mod = NULL;

        if (mm == NULL) {
            return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
        }

        mod = new OpenAMHttpModule(status);
        if (mod == NULL) {
            return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
        }

        *mm = mod;
        mod = NULL;

        return S_OK;
    }

    virtual void Terminate() {
        am_shutdown(AM_DEFAULT_AGENT_ID);
        delete this;
    }
    
    private:
        int status;
};

HRESULT __stdcall RegisterModule(DWORD dwServerVersion,
        IHttpModuleRegistrationInfo *pModuleInfo, IHttpServer *pHttpServer) {
    HRESULT status = S_OK;
    OpenAMHttpModuleFactory *modf = NULL;
    UNREFERENCED_PARAMETER(dwServerVersion);

    do {

        if (pModuleInfo == NULL || pHttpServer == NULL) {
            status = HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
            break;
        }

        modctx = pModuleInfo->GetId();
        server = pHttpServer;

        modf = new OpenAMHttpModuleFactory();
        if (modf == NULL) {
            status = HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
            break;
        }

        status = pModuleInfo->SetRequestNotifications(modf,
                RQ_BEGIN_REQUEST | RQ_AUTHENTICATE_REQUEST | RQ_END_REQUEST, 0);
        if (FAILED(status)) {
            break;
        }
        status = pModuleInfo->SetPriorityForRequestNotification(RQ_BEGIN_REQUEST,
                PRIORITY_ALIAS_HIGH);
        if (FAILED(status)) {
            break;
        }

        modf = NULL;

    } while (FALSE);

    if (modf != NULL) {
        delete modf;
        modf = NULL;
    }

    return status;
}
