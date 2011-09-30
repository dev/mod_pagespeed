/*
 * Copyright 2010 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Author: jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/rewriter/public/cache_extender.h"

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/http/public/meta_data.h"
#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/css_tag_scanner.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/domain_rewrite_filter.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/output_resource_kind.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_slot.h"
#include "net/instaweb/rewriter/public/resource_tag_scanner.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_single_resource_filter.h"
#include "net/instaweb/rewriter/public/single_rewrite_context.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/ref_counted_ptr.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/string_writer.h"
#include "net/instaweb/util/public/timer.h"

namespace {

// names for Statistics variables.
const char kCacheExtensions[] = "cache_extensions";
const char kNotCacheable[] = "not_cacheable";

}  // namespace

namespace net_instaweb {
class MessageHandler;
class RewriteContext;

// We do not want to bother to extend the cache lifetime for any resource
// that is already cached for a month.
const int64 kMinThresholdMs = Timer::kMonthMs;

class CacheExtender::Context : public SingleRewriteContext {
 public:
  Context(CacheExtender* extender, RewriteDriver* driver,
          RewriteContext* parent)
      : SingleRewriteContext(driver, parent,
                             NULL /* no resource context */),
        extender_(extender),
        driver_(driver) {}
  virtual ~Context() {}

  virtual void Render();
  virtual void RewriteSingle(const ResourcePtr& input,
                             const OutputResourcePtr& output);
  virtual const char* id() const { return extender_->id().c_str(); }
  virtual OutputResourceKind kind() const { return kOnTheFlyResource; }

 private:
  CacheExtender* extender_;
  RewriteDriver* driver_;
  DISALLOW_COPY_AND_ASSIGN(Context);
};

CacheExtender::CacheExtender(RewriteDriver* driver, const char* filter_prefix)
    : RewriteSingleResourceFilter(driver, filter_prefix),
      tag_scanner_(driver_),
      extension_count_(NULL),
      not_cacheable_count_(NULL) {
  Statistics* stats = resource_manager_->statistics();
  if (stats != NULL) {
    extension_count_ = stats->GetVariable(kCacheExtensions);
    not_cacheable_count_ = stats->GetVariable(kNotCacheable);
  }
}

CacheExtender::~CacheExtender() {}

void CacheExtender::Initialize(Statistics* statistics) {
  statistics->AddVariable(kCacheExtensions);
  statistics->AddVariable(kNotCacheable);
}

bool CacheExtender::ShouldRewriteResource(
    const ResponseHeaders* headers, int64 now_ms,
    const ResourcePtr& input_resource, const StringPiece& url) const {
  if (input_resource->type() == NULL) {
    return false;
  }
  if ((headers->CacheExpirationTimeMs() - now_ms) < kMinThresholdMs) {
    // This also includes the case where a previous filter rewrote this.
    return true;
  }
  GoogleUrl origin_gurl(url);
  StringPiece origin = origin_gurl.Origin();
  const DomainLawyer* lawyer = driver_->options()->domain_lawyer();
  return lawyer->WillDomainChange(origin);
}

void CacheExtender::StartElementImpl(HtmlElement* element) {
  // Disable extend_cache for img if ModPagespeedDisableForBots is on
  // and the user-agent is a bot.
  if (element->keyword() == HtmlName::kImg &&
      driver_->ShouldNotRewriteImages()) {
    return;
  }
  HtmlElement::Attribute* href = tag_scanner_.ScanElement(element);

  // TODO(jmarantz): We ought to be able to domain-shard even if the
  // resources are non-cacheable or privately cacheable.
  if ((href != NULL) && driver_->IsRewritable(element)) {
    ResourcePtr input_resource(CreateInputResource(href->value()));
    if (input_resource.get() == NULL) {
      return;
    }

    GoogleUrl input_gurl(input_resource->url());
    if (resource_manager_->IsPagespeedResource(input_gurl)) {
      return;
    }

    if (HasAsyncFlow()) {
      ResourceSlotPtr slot(driver_->GetSlot(input_resource, element, href));
      Context* context = new Context(this, driver_, NULL /* not nested */);
      context->AddSlot(slot);
      driver_->InitiateRewrite(context);
    } else {
      scoped_ptr<CachedResult> rewrite_info(
          RewriteExternalResource(input_resource, NULL));
      if (rewrite_info.get() != NULL && rewrite_info->optimizable()) {
        // Rewrite URL to cache-extended version
        href->SetValue(rewrite_info->url());
        extension_count_->Add(1);
      }
    }
  }
}

bool CacheExtender::ComputeOnTheFly() const {
  return true;
}

void CacheExtender::Context::RewriteSingle(
    const ResourcePtr& input_resource,
    const OutputResourcePtr& output_resource) {
  RewriteDone(
      extender_->RewriteLoadedResource(input_resource, output_resource), 0);
}

void CacheExtender::Context::Render() {
  extender_->extension_count_->Add(1);
}

RewriteSingleResourceFilter::RewriteResult CacheExtender::RewriteLoadedResource(
    const ResourcePtr& input_resource,
    const OutputResourcePtr& output_resource) {
  CHECK(input_resource->loaded());

  MessageHandler* message_handler = driver_->message_handler();
  const ResponseHeaders* headers = input_resource->response_headers();
  GoogleString url = input_resource->url();
  int64 now_ms = resource_manager_->timer()->NowMs();

  // See if the resource is cacheable; and if so whether there is any need
  // to cache extend it.
  bool ok = false;
  if (!resource_manager_->http_cache()->force_caching() &&
      !headers->IsCacheable()) {
    not_cacheable_count_->Add(1);
  } else if (ShouldRewriteResource(headers, now_ms, input_resource, url)) {
    output_resource->SetType(input_resource->type());
    ok = true;
  }

  if (!ok) {
    return RewriteSingleResourceFilter::kRewriteFailed;
  }

  StringPiece contents(input_resource->contents());
  GoogleString transformed_contents;
  GoogleUrl input_resource_gurl(input_resource->url());
  StringPiece input_dir = input_resource_gurl.AllExceptLeaf();
  const DomainLawyer* lawyer = driver_->options()->domain_lawyer();
  if ((input_resource->type() == &kContentTypeCss) &&
      (lawyer->WillDomainChange(input_resource_gurl.Origin()) ||
       (input_dir != output_resource->resolved_base()))) {
    GoogleUrl output_base(output_resource->resolved_base());
    if (output_base.is_valid()) {
      // TODO(jmarantz): find a mechanism to write this directly into
      // the HTTPValue so we can reduce the number of times that we
      // copy entire resources.
      StringWriter writer(&transformed_contents);
      RewriteDomainTransformer transformer(&input_resource_gurl, &output_base,
                                           driver_);
      CssTagScanner::TransformUrls(contents, &writer, &transformer,
                                   message_handler);
      contents = transformed_contents;
    }
  }

  resource_manager_->MergeNonCachingResponseHeaders(
      input_resource, output_resource);
  if (resource_manager_->Write(
          HttpStatus::kOK, contents, output_resource.get(),
          headers->CacheExpirationTimeMs(), message_handler)) {
    return RewriteSingleResourceFilter::kRewriteOk;
  } else {
    return RewriteSingleResourceFilter::kRewriteFailed;
  }
}

bool CacheExtender::HasAsyncFlow() const {
  return driver_->asynchronous_rewrites();
}

RewriteContext* CacheExtender::MakeRewriteContext() {
  return new Context(this, driver_, NULL /*not nested*/);
}

RewriteContext* CacheExtender::MakeNestedContext(
    RewriteContext* parent, const ResourceSlotPtr& slot) {
  Context* context = new Context(this, NULL /* driver*/, parent);
  context->AddSlot(slot);
  return context;
}

}  // namespace net_instaweb
