// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/media/desktop_media_list_ash.h"

#include <map>

#include "ash/shell.h"
#include "ash/shell_window_ids.h"
#include "base/hash.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/sequenced_worker_pool.h"
#include "chrome/browser/media/desktop_media_list_observer.h"
#include "content/public/browser/browser_thread.h"
#include "grit/generated_resources.h"
#include "media/base/video_util.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/compositor/dip_util.h"
#include "ui/gfx/image/image.h"
#include "ui/snapshot/snapshot.h"

using content::BrowserThread;
using content::DesktopMediaID;

namespace {

// Update the list twice per second.
const int kDefaultUpdatePeriod = 500;

}  // namespace

DesktopMediaListAsh::SourceDescription::SourceDescription(
    DesktopMediaID id,
    const base::string16& name)
    : id(id),
      name(name) {
}

DesktopMediaListAsh::DesktopMediaListAsh(int source_types)
    : source_types_(source_types),
      update_period_(base::TimeDelta::FromMilliseconds(kDefaultUpdatePeriod)),
      thumbnail_size_(100, 100),
      view_dialog_id_(-1),
      observer_(NULL),
      pending_window_capture_requests_(0),
      weak_factory_(this) {
}

DesktopMediaListAsh::~DesktopMediaListAsh() {}

void DesktopMediaListAsh::SetUpdatePeriod(base::TimeDelta period) {
  DCHECK(!observer_);
  update_period_ = period;
}

void DesktopMediaListAsh::SetThumbnailSize(
    const gfx::Size& thumbnail_size) {
  thumbnail_size_ = thumbnail_size;
}

void DesktopMediaListAsh::SetViewDialogWindowId(
    content::DesktopMediaID::Id dialog_id) {
  view_dialog_id_ = dialog_id;
}

void DesktopMediaListAsh::StartUpdating(DesktopMediaListObserver* observer) {
  DCHECK(!observer_);

  observer_ = observer;
  Refresh();
}

int DesktopMediaListAsh::GetSourceCount() const {
  return sources_.size();
}

const DesktopMediaList::Source& DesktopMediaListAsh::GetSource(
    int index) const {
  return sources_[index];
}

// static
bool DesktopMediaListAsh::CompareSources(const SourceDescription& a,
                                         const SourceDescription& b) {
  return a.id < b.id;
}

void DesktopMediaListAsh::Refresh() {
  std::vector<SourceDescription> new_sources;
  EnumerateSources(&new_sources);

  // Sort the list of sources so that they appear in a predictable order.
  std::sort(new_sources.begin(), new_sources.end(), CompareSources);

  // Step through |new_sources| adding and removing entries from |sources_|, and
  // notifying the |observer_|, until two match. Requires that |sources| and
  // |sources_| have the same ordering.
  size_t pos = 0;
  while (pos < sources_.size() || pos < new_sources.size()) {
    // If |sources_[pos]| is not in |new_sources| then remove it.
    if (pos < sources_.size() &&
        (pos == new_sources.size() || sources_[pos].id < new_sources[pos].id)) {
      sources_.erase(sources_.begin() + pos);
      observer_->OnSourceRemoved(pos);
      continue;
    }

    if (pos == sources_.size() || !(sources_[pos].id == new_sources[pos].id)) {
      sources_.insert(sources_.begin() + pos, Source());
      sources_[pos].id = new_sources[pos].id;
      sources_[pos].name = new_sources[pos].name;
      observer_->OnSourceAdded(pos);
    } else if (sources_[pos].name != new_sources[pos].name) {
      sources_[pos].name = new_sources[pos].name;
      observer_->OnSourceNameChanged(pos);
    }

    ++pos;
  }

  DCHECK_EQ(new_sources.size(), sources_.size());
}

void DesktopMediaListAsh::EnumerateWindowsForRoot(
    std::vector<DesktopMediaListAsh::SourceDescription>* sources,
    aura::Window* root_window,
    int container_id) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));

  aura::Window* container = ash::Shell::GetContainer(root_window, container_id);
  if (!container)
    return;
  for (aura::Window::Windows::const_iterator it = container->children().begin();
       it != container->children().end(); ++it) {
    if (!(*it)->IsVisible() || !(*it)->CanFocus())
      continue;
    content::DesktopMediaID id =
        content::DesktopMediaID::RegisterAuraWindow(*it);
    if (id.id == view_dialog_id_)
      continue;
    SourceDescription window_source(id, (*it)->title());
    sources->push_back(window_source);

    CaptureThumbnail(window_source.id, *it);
  }
}

void DesktopMediaListAsh::EnumerateSources(
    std::vector<DesktopMediaListAsh::SourceDescription>* sources) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));

  aura::Window::Windows root_windows = ash::Shell::GetAllRootWindows();

  for (aura::Window::Windows::const_iterator iter = root_windows.begin();
       iter != root_windows.end(); ++iter) {
    if (source_types_ & SCREENS) {
      SourceDescription screen_source(
          content::DesktopMediaID::RegisterAuraWindow(*iter), (*iter)->title());
      sources->push_back(screen_source);

      CaptureThumbnail(screen_source.id, *iter);
    }

    if (source_types_ & WINDOWS) {
      EnumerateWindowsForRoot(
          sources, *iter, ash::internal::kShellWindowId_DefaultContainer);
      EnumerateWindowsForRoot(
          sources, *iter, ash::internal::kShellWindowId_AlwaysOnTopContainer);
      EnumerateWindowsForRoot(
          sources, *iter, ash::internal::kShellWindowId_DockedContainer);
    }
  }
}

void DesktopMediaListAsh::CaptureThumbnail(content::DesktopMediaID id,
                                           aura::Window* window) {
  gfx::Rect window_rect(window->bounds().width(), window->bounds().height());
  gfx::Rect scaled_rect = media::ComputeLetterboxRegion(
      gfx::Rect(thumbnail_size_), window_rect.size());

  ++pending_window_capture_requests_;
  ui::GrabWindowSnapshotAsync(
      window, window_rect,
      scaled_rect.size(),
      BrowserThread::GetBlockingPool(),
      base::Bind(&DesktopMediaListAsh::OnThumbnailCaptured,
                 weak_factory_.GetWeakPtr(),
                 id));
}

void DesktopMediaListAsh::OnThumbnailCaptured(content::DesktopMediaID id,
                                              const gfx::Image& image) {
  for (size_t i = 0; i < sources_.size(); ++i) {
    if (sources_[i].id == id) {
      sources_[i].thumbnail = image.AsImageSkia();
      observer_->OnSourceThumbnailChanged(i);
      break;
    }
  }

  --pending_window_capture_requests_;
  DCHECK_GE(pending_window_capture_requests_, 0);

  if (!pending_window_capture_requests_) {
    // Once we've finished capturing all windows post a task for the next list
    // update.
    BrowserThread::PostDelayedTask(
        BrowserThread::UI, FROM_HERE,
        base::Bind(&DesktopMediaListAsh::Refresh,
                   weak_factory_.GetWeakPtr()),
        update_period_);
  }
}
