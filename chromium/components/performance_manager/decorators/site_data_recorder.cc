// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/performance_manager/public/decorators/site_data_recorder.h"

#include "base/time/time.h"
#include "components/performance_manager/graph/node_attached_data_impl.h"
#include "components/performance_manager/graph/page_node_impl.h"
#include "components/performance_manager/persistence/site_data/site_data_cache.h"
#include "components/performance_manager/persistence/site_data/site_data_cache_factory.h"
#include "components/performance_manager/persistence/site_data/site_data_writer.h"
#include "components/performance_manager/public/persistence/site_data/site_data_reader.h"

namespace performance_manager {

// The period of time after loading during which we ignore title/favicon
// change events. It's possible for some site that are loaded in background to
// use some of these features without this being an attempt to communicate
// with the user (e.g. the page is just really finishing to load).
constexpr base::TimeDelta kTitleOrFaviconChangePostLoadGracePeriod =
    base::TimeDelta::FromSeconds(20);

// The period of time during which audio usage gets ignored after a page gets
// backgrounded. It's necessary because there might be a delay between a media
// request gets initiated and the time the audio actually starts.
constexpr base::TimeDelta kFeatureUsagePostBackgroundGracePeriod =
    base::TimeDelta::FromSeconds(10);

// Provides SiteData machinery access to some internals of a PageNodeImpl.
class SiteDataAccess {
 public:
  static std::unique_ptr<NodeAttachedData>* GetUniquePtrStorage(
      PageNodeImpl* page_node) {
    return &page_node->site_data_;
  }
};

namespace {

// NodeAttachedData used to adorn every page node with a SiteDataWriter.
class SiteDataNodeData : public NodeAttachedDataImpl<SiteDataNodeData>,
                         public SiteDataRecorder::Data {
 public:
  struct Traits : public NodeAttachedDataOwnedByNodeType<PageNodeImpl> {};

  SiteDataNodeData() = default;
  explicit SiteDataNodeData(const PageNodeImpl* page_node)
      : page_node_(page_node) {}
  ~SiteDataNodeData() override = default;

  // NodeAttachedData:
  static std::unique_ptr<NodeAttachedData>* GetUniquePtrStorage(
      PageNodeImpl* page_node) {
    return SiteDataAccess::GetUniquePtrStorage(page_node);
  }

  // Set the SiteDataCache that should be used to create the writer.
  void set_data_cache(SiteDataCache* data_cache) {
    DCHECK(data_cache);
    data_cache_ = data_cache;
  }

  // Functions called whenever one of the tracked properties changes.
  void OnMainFrameUrlChanged(const GURL& url, bool page_is_visible);
  void OnIsLoadingChanged(bool is_loading);
  void OnIsVisibleChanged(bool is_visible);
  void OnIsAudibleChanged(bool audible);
  void OnTitleUpdated();
  void OnFaviconUpdated();

  void Reset();

  SiteDataWriter* writer() const override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return writer_.get();
  }

  SiteDataReader* reader() const override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return reader_.get();
  }

 private:
  // The features tracked by the SiteDataRecorder class.
  enum class FeatureType {
    kTitleChange,
    kFaviconChange,
    kAudioUsage,
  };

  void SetDataCacheForTesting(SiteDataCache* cache) override {
    set_data_cache(cache);
  }

  // Indicates of a feature usage event should be ignored.
  bool ShouldIgnoreFeatureUsageEvent(FeatureType feature_type);

  // Records a feature usage event if necessary.
  void MaybeNotifyBackgroundFeatureUsage(void (SiteDataWriter::*method)(),
                                         FeatureType feature_type);

  TabVisibility GetPageNodeVisibility() {
    return page_node_->is_visible() ? TabVisibility::kForeground
                                    : TabVisibility::kBackground;
  }

  // The SiteDataCache used to serve writers for the PageNode owned by this
  // object.
  SiteDataCache* data_cache_ = nullptr;

  // The PageNode that owns this object.
  const PageNodeImpl* page_node_ = nullptr;

  // The time at which this tab switched to the loaded state, null if this tab
  // is not currently loaded.
  base::TimeTicks loaded_time_;

  std::unique_ptr<SiteDataWriter> writer_;
  std::unique_ptr<SiteDataReader> reader_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(SiteDataNodeData);
};

void SiteDataNodeData::OnMainFrameUrlChanged(const GURL& url,
                                             bool page_is_visible) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  url::Origin origin = url::Origin::Create(url);

  if (writer_ && origin == writer_->Origin())
    return;

  // If the origin has changed then the writer should be invalidated.
  Reset();

  if (!url.SchemeIsHTTPOrHTTPS())
    return;

  writer_ = data_cache_->GetWriterForOrigin(origin);
  reader_ = data_cache_->GetReaderForOrigin(origin);

  // The writer is assumed to be in an unloaded state by default, set the proper
  // loading state if necessary.
  if (!page_node_->is_loading())
    OnIsLoadingChanged(false);
}

void SiteDataNodeData::OnIsLoadingChanged(bool is_loading) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!writer_)
    return;
  if (is_loading && !loaded_time_.is_null()) {
    writer_->NotifySiteUnloaded(GetPageNodeVisibility());
    loaded_time_ = base::TimeTicks();
  } else if (!is_loading) {
    writer_->NotifySiteLoaded(GetPageNodeVisibility());
    loaded_time_ = base::TimeTicks::Now();
  }
}

void SiteDataNodeData::OnIsVisibleChanged(bool is_visible) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!writer_)
    return;
  if (is_visible) {
    writer_->NotifySiteForegrounded(!page_node_->is_loading());
  } else {
    writer_->NotifySiteBackgrounded(!page_node_->is_loading());
  }
}

void SiteDataNodeData::OnIsAudibleChanged(bool audible) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!audible)
    return;

  MaybeNotifyBackgroundFeatureUsage(
      &SiteDataWriter::NotifyUsesAudioInBackground, FeatureType::kAudioUsage);
}

void SiteDataNodeData::OnTitleUpdated() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  MaybeNotifyBackgroundFeatureUsage(
      &SiteDataWriter::NotifyUpdatesTitleInBackground,
      FeatureType::kTitleChange);
}

void SiteDataNodeData::OnFaviconUpdated() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  MaybeNotifyBackgroundFeatureUsage(
      &SiteDataWriter::NotifyUpdatesFaviconInBackground,
      FeatureType::kFaviconChange);
}

void SiteDataNodeData::Reset() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (writer_ && !loaded_time_.is_null() && !page_node_->is_loading()) {
    writer_->NotifySiteUnloaded(GetPageNodeVisibility());
    loaded_time_ = base::TimeTicks();
  }
  writer_.reset();
  reader_.reset();
}

bool SiteDataNodeData::ShouldIgnoreFeatureUsageEvent(FeatureType feature_type) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // The feature usage should be ignored if there's no writer for this page.
  if (!writer_)
    return true;

  // Ignore all features happening before the website gets fully loaded.
  if (page_node_->is_loading())
    return true;

  // Ignore events if the tab is not in background.
  if (GetPageNodeVisibility() == TabVisibility::kForeground)
    return true;

  if (feature_type == FeatureType::kTitleChange ||
      feature_type == FeatureType::kFaviconChange) {
    DCHECK(!loaded_time_.is_null());
    if (base::TimeTicks::Now() - loaded_time_ <
        kTitleOrFaviconChangePostLoadGracePeriod) {
      return true;
    }
  }

  // Ignore events happening shortly after the tab being backgrounded, they're
  // usually false positives.
  if ((page_node_->TimeSinceLastVisibilityChange() <
       kFeatureUsagePostBackgroundGracePeriod)) {
    return true;
  }

  return false;
}

void SiteDataNodeData::MaybeNotifyBackgroundFeatureUsage(
    void (SiteDataWriter::*method)(),
    FeatureType feature_type) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (ShouldIgnoreFeatureUsageEvent(feature_type))
    return;

  (writer_.get()->*method)();
}

SiteDataNodeData* GetSiteDataNodeDataFromPageNode(const PageNode* page_node) {
  auto* page_node_impl = PageNodeImpl::FromNode(page_node);
  DCHECK(page_node_impl);
  auto* data = SiteDataNodeData::Get(page_node_impl);
  DCHECK(data);
  return data;
}

}  // namespace

SiteDataRecorder::SiteDataRecorder() {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

SiteDataRecorder::~SiteDataRecorder() = default;

void SiteDataRecorder::OnPassedToGraph(Graph* graph) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  RegisterObservers(graph);
}

void SiteDataRecorder::OnTakenFromGraph(Graph* graph) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  UnregisterObservers(graph);
}

void SiteDataRecorder::OnPageNodeAdded(const PageNode* page_node) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  SetPageNodeDataCache(page_node);
}

void SiteDataRecorder::OnBeforePageNodeRemoved(const PageNode* page_node) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto* data = GetSiteDataNodeDataFromPageNode(page_node);
  data->Reset();
}

void SiteDataRecorder::OnMainFrameUrlChanged(const PageNode* page_node) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto* data = GetSiteDataNodeDataFromPageNode(page_node);
  data->OnMainFrameUrlChanged(page_node->GetMainFrameUrl(),
                              page_node->IsVisible());
}

void SiteDataRecorder::OnIsLoadingChanged(const PageNode* page_node) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto* data = GetSiteDataNodeDataFromPageNode(page_node);
  data->OnIsLoadingChanged(page_node->IsLoading());
}

void SiteDataRecorder::OnIsVisibleChanged(const PageNode* page_node) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto* data = GetSiteDataNodeDataFromPageNode(page_node);
  data->OnIsVisibleChanged(page_node->IsVisible());
}

void SiteDataRecorder::OnIsAudibleChanged(const PageNode* page_node) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto* data = GetSiteDataNodeDataFromPageNode(page_node);
  data->OnIsAudibleChanged(page_node->IsAudible());
}

void SiteDataRecorder::OnTitleUpdated(const PageNode* page_node) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto* data = GetSiteDataNodeDataFromPageNode(page_node);
  data->OnTitleUpdated();
}

void SiteDataRecorder::OnFaviconUpdated(const PageNode* page_node) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto* data = GetSiteDataNodeDataFromPageNode(page_node);
  data->OnFaviconUpdated();
}

void SiteDataRecorder::RegisterObservers(Graph* graph) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  graph->AddPageNodeObserver(this);
}

void SiteDataRecorder::UnregisterObservers(Graph* graph) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  graph->RemovePageNodeObserver(this);
}

void SiteDataRecorder::SetPageNodeDataCache(const PageNode* page_node) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto* page_node_impl = PageNodeImpl::FromNode(page_node);
  DCHECK(page_node_impl);
  DCHECK(!SiteDataNodeData::Get(page_node_impl));
  auto* data = SiteDataNodeData::GetOrCreate(page_node_impl);
  data->set_data_cache(
      SiteDataCacheFactory::GetInstance()->GetDataCacheForBrowserContext(
          page_node->GetBrowserContextID()));
}

SiteDataRecorder::Data::Data() = default;
SiteDataRecorder::Data::~Data() = default;

// static
const SiteDataRecorder::Data* SiteDataRecorder::Data::FromPageNode(
    const PageNode* page_node) {
  return SiteDataNodeData::Get(PageNodeImpl::FromNode(page_node));
}

// static
SiteDataRecorder::Data* SiteDataRecorder::Data::GetForTesting(
    const PageNode* page_node) {
  return GetSiteDataNodeDataFromPageNode(page_node);
}

}  // namespace performance_manager
