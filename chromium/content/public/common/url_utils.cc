// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/common/url_utils.h"

#include <set>
#include <string>

#include "base/check_op.h"
#include "base/containers/flat_set.h"
#include "base/feature_list.h"
#include "base/no_destructor.h"
#include "base/strings/string_piece.h"
#include "content/common/url_schemes.h"
#include "content/public/common/content_features.h"
#include "content/public/common/url_constants.h"
#include "url/gurl.h"
#include "url/url_util.h"
#include "url/url_util_qt.h"

namespace content {

bool HasWebUIScheme(const GURL& url) {
  return url.SchemeIs(kChromeDevToolsScheme) || url.SchemeIs(kChromeUIScheme) ||
         url.SchemeIs(kChromeUIUntrustedScheme);
}

bool IsSavableURL(const GURL& url) {
  for (auto& scheme : GetSavableSchemes()) {
    if (url.SchemeIs(scheme))
      return true;
  }
  return false;
}

bool IsURLHandledByNetworkStack(const GURL& url) {
  // Javascript URLs, srcdoc, schemes that don't load data should not send a
  // request to the network stack.
  if (url.SchemeIs(url::kJavaScriptScheme) || url.is_empty() ||
      url.IsAboutSrcdoc()) {
    return false;
  }

  for (const auto& scheme : url::GetEmptyDocumentSchemes()) {
    if (url.SchemeIs(scheme))
      return false;
  }

  // Renderer debug URLs (e.g. chrome://kill) are handled in the renderer
  // process directly and should not be sent to the network stack.
  if (IsRendererDebugURL(url))
    return false;

  // For you information, even though a "data:" url doesn't generate actual
  // network requests, it is handled by the network stack and so must return
  // true. The reason is that a few "data:" urls can't be handled locally. For
  // instance:
  // - the ones that result in downloads.
  // - the ones that are invalid. An error page must be served instead.
  // - the ones that have an unsupported MIME type.
  // - the ones that target the top-level frame on Android.

  return true;
}

bool IsRendererDebugURL(const GURL& url) {
  if (!url.is_valid())
    return false;

  if (url.SchemeIs(url::kJavaScriptScheme))
    return true;

  if (!url.SchemeIs(kChromeUIScheme))
    return false;

  if (url == kChromeUICheckCrashURL || url == kChromeUIBadCastCrashURL ||
      url == kChromeUICrashURL || url == kChromeUIDumpURL ||
      url == kChromeUIKillURL || url == kChromeUIHangURL ||
      url == kChromeUIShorthangURL || url == kChromeUIMemoryExhaustURL) {
    return true;
  }

#if defined(ADDRESS_SANITIZER)
  if (url == kChromeUICrashHeapOverflowURL ||
      url == kChromeUICrashHeapUnderflowURL ||
      url == kChromeUICrashUseAfterFreeURL) {
    return true;
  }
#endif

#if defined(OS_WIN)
  if (url == kChromeUIHeapCorruptionCrashURL)
    return true;
#endif

#if DCHECK_IS_ON()
  if (url == kChromeUICrashDcheckURL)
    return true;
#endif

#if defined(OS_WIN) && defined(ADDRESS_SANITIZER)
  if (url == kChromeUICrashCorruptHeapBlockURL ||
      url == kChromeUICrashCorruptHeapURL) {
    return true;
  }
#endif

  return false;
}

bool IsSafeRedirectTarget(const GURL& from_url, const GURL& to_url) {
  static const base::NoDestructor<base::flat_set<base::StringPiece>>
      kUnsafeSchemes(base::flat_set<base::StringPiece>({
        url::kAboutScheme,
            url::kBlobScheme,
            url::kJavaScriptScheme,
#if !defined(CHROMECAST_BUILD)
            url::kDataScheme,
#endif
#if defined(OS_ANDROID)
            url::kContentScheme,
#endif
      }));
  if (from_url.is_empty())
    return false;
  static const auto& sLocalSchemesList = url::GetLocalSchemes();
  static const base::NoDestructor<base::flat_set<base::StringPiece>>
      sLocalSchemes(base::flat_set<base::StringPiece>(sLocalSchemesList.begin(), sLocalSchemesList.end()));
  if (sLocalSchemes->contains(to_url.scheme_piece())) {
#if defined(TOOLKIT_QT)
    if (auto *cs = url::CustomScheme::FindScheme(from_url.scheme_piece())) {
      if (cs->flags & (url::CustomScheme::Local | url::CustomScheme::LocalAccessAllowed))
        return true;
    }
#endif
    return sLocalSchemes->contains(from_url.scheme_piece());
  }
#if defined(TOOLKIT_QT)
  if (from_url.IsCustom())
    return true;
#endif
  if (HasWebUIScheme(to_url))
    return false;
  if (kUnsafeSchemes->contains(to_url.scheme_piece()))
    return false;
  if (to_url.SchemeIsFileSystem())
    return from_url.SchemeIsFileSystem();
  return true;
}

}  // namespace content
