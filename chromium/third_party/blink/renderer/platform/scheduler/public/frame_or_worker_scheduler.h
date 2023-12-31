// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_SCHEDULER_PUBLIC_FRAME_OR_WORKER_SCHEDULER_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_SCHEDULER_PUBLIC_FRAME_OR_WORKER_SCHEDULER_H_

#include "base/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/util/type_safety/strong_alias.h"
#include "third_party/blink/renderer/platform/platform_export.h"
#include "third_party/blink/renderer/platform/scheduler/public/scheduling_lifecycle_state.h"
#include "third_party/blink/renderer/platform/scheduler/public/scheduling_policy.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"
#include "third_party/blink/renderer/platform/wtf/hash_map.h"

namespace blink {
class FrameScheduler;

// This is the base class of FrameScheduler and WorkerScheduler.
class PLATFORM_EXPORT FrameOrWorkerScheduler {
  USING_FAST_MALLOC(FrameOrWorkerScheduler);

 public:
  // Observer type that regulates conditions to invoke callbacks.
  enum class ObserverType { kLoader, kWorkerScheduler };

  // Callback type for receiving scheduling policy change events.
  using OnLifecycleStateChangedCallback =
      base::RepeatingCallback<void(scheduler::SchedulingLifecycleState)>;

  class PLATFORM_EXPORT LifecycleObserverHandle {
    USING_FAST_MALLOC(LifecycleObserverHandle);

   public:
    explicit LifecycleObserverHandle(FrameOrWorkerScheduler* scheduler);
    ~LifecycleObserverHandle();

   private:
    base::WeakPtr<FrameOrWorkerScheduler> scheduler_;

    DISALLOW_COPY_AND_ASSIGN(LifecycleObserverHandle);
  };

  // RAII handle which should be kept alive as long as the feature is active
  // and the policy should be applied.
  class PLATFORM_EXPORT SchedulingAffectingFeatureHandle {
    DISALLOW_NEW();

   public:
    SchedulingAffectingFeatureHandle() = default;
    SchedulingAffectingFeatureHandle(SchedulingAffectingFeatureHandle&&);
    inline ~SchedulingAffectingFeatureHandle() { reset(); }

    SchedulingAffectingFeatureHandle& operator=(
        SchedulingAffectingFeatureHandle&&);

    explicit operator bool() const { return scheduler_.get(); }

    inline void reset() {
      if (scheduler_)
        scheduler_->OnStoppedUsingFeature(feature_, policy_);
      scheduler_ = nullptr;
    }

   private:
    friend class FrameOrWorkerScheduler;

    SchedulingAffectingFeatureHandle(SchedulingPolicy::Feature feature,
                                     SchedulingPolicy policy,
                                     base::WeakPtr<FrameOrWorkerScheduler>);

    SchedulingPolicy::Feature feature_ = SchedulingPolicy::Feature::kMaxValue;
    SchedulingPolicy policy_;
    base::WeakPtr<FrameOrWorkerScheduler> scheduler_;

    DISALLOW_COPY_AND_ASSIGN(SchedulingAffectingFeatureHandle);
  };

  virtual ~FrameOrWorkerScheduler();

  using Preempted = util::StrongAlias<class PreemptedTag, bool>;
  // Stops any tasks from running while we yield and run a nested loop.
  virtual void SetPreemptedForCooperativeScheduling(Preempted) = 0;

  // Notifies scheduler that this execution context has started using a feature
  // which impacts scheduling decisions.
  // When the feature stops being used, this handle should be destroyed.
  //
  // Usage:
  // handle = scheduler->RegisterFeature(
  //     kYourFeature, { SchedulingPolicy::DisableSomething() });
  SchedulingAffectingFeatureHandle RegisterFeature(
      SchedulingPolicy::Feature feature,
      SchedulingPolicy policy) WARN_UNUSED_RESULT;

  // Register a feature which is used for the rest of the lifetime of
  // the document and can't be unregistered.
  // The policy is reset when the main frame navigates away from the current
  // document.
  void RegisterStickyFeature(SchedulingPolicy::Feature feature,
                             SchedulingPolicy policy);

  // Adds an observer callback to be notified on scheduling policy changed.
  // When a callback is added, the initial state will be notified synchronously
  // through the callback. The callback may be invoked consecutively with the
  // same value. Returns a RAII handle that unregisters the callback when the
  // handle is destroyed.
  //
  // New usage outside of platform/ should be rare. Prefer using
  // ExecutionContextLifecycleStateObserver to observe paused and frozenness
  // changes and PageVisibilityObserver to observe visibility changes. One
  // exception is that this observer enables observing visibility changes of the
  // associated page in workers, whereas PageVisibilityObserver does not
  // (crbug.com/1286570).
  std::unique_ptr<LifecycleObserverHandle> AddLifecycleObserver(
      ObserverType,
      OnLifecycleStateChangedCallback) WARN_UNUSED_RESULT;

  virtual FrameScheduler* ToFrameScheduler() { return nullptr; }

  base::WeakPtr<FrameOrWorkerScheduler> GetWeakPtr();

 protected:
  FrameOrWorkerScheduler();

  void NotifyLifecycleObservers();

  virtual scheduler::SchedulingLifecycleState CalculateLifecycleState(
      ObserverType) const {
    return scheduler::SchedulingLifecycleState::kNotThrottled;
  }

  virtual void OnStartedUsingFeature(SchedulingPolicy::Feature feature,
                                     const SchedulingPolicy& policy) = 0;
  virtual void OnStoppedUsingFeature(SchedulingPolicy::Feature feature,
                                     const SchedulingPolicy& policy) = 0;

  virtual base::WeakPtr<FrameOrWorkerScheduler> GetDocumentBoundWeakPtr();

 private:
  class ObserverState {
   public:
    ObserverState(ObserverType, OnLifecycleStateChangedCallback);
    ObserverState(const ObserverState&) = delete;
    ObserverState& operator=(const ObserverState&) = delete;
    ~ObserverState();

    ObserverType GetObserverType() const { return observer_type_; }
    OnLifecycleStateChangedCallback& GetCallback() { return callback_; }

   private:
    ObserverType observer_type_;
    OnLifecycleStateChangedCallback callback_;
  };

  void RemoveLifecycleObserver(LifecycleObserverHandle* handle);

  HashMap<LifecycleObserverHandle*, std::unique_ptr<ObserverState>>
      lifecycle_observers_;
  base::WeakPtrFactory<FrameOrWorkerScheduler> weak_factory_{this};
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_SCHEDULER_PUBLIC_FRAME_OR_WORKER_SCHEDULER_H_
