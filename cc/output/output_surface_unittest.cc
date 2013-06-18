// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/test/test_simple_task_runner.h"
#include "cc/output/output_surface.h"
#include "cc/output/output_surface_client.h"
#include "cc/output/software_output_device.h"
#include "cc/test/scheduler_test_common.h"
#include "cc/test/test_web_graphics_context_3d.h"
#include "gpu/GLES2/gl2extchromium.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace cc {
namespace {

class TestOutputSurface : public OutputSurface {
 public:
  explicit TestOutputSurface(scoped_ptr<WebKit::WebGraphicsContext3D> context3d)
      : OutputSurface(context3d.Pass()) {}

  explicit TestOutputSurface(
      scoped_ptr<cc::SoftwareOutputDevice> software_device)
      : OutputSurface(software_device.Pass()) {}

  TestOutputSurface(scoped_ptr<WebKit::WebGraphicsContext3D> context3d,
                    scoped_ptr<cc::SoftwareOutputDevice> software_device)
      : OutputSurface(context3d.Pass(), software_device.Pass()) {}

  bool InitializeNewContext3D(
      scoped_ptr<WebKit::WebGraphicsContext3D> new_context3d) {
    return InitializeAndSetContext3D(new_context3d.Pass(),
                                     scoped_refptr<ContextProvider>());
  }

  bool HasClientForTesting() {
    return HasClient();
  }

  void OnVSyncParametersChangedForTesting(base::TimeTicks timebase,
                                          base::TimeDelta interval) {
    OnVSyncParametersChanged(timebase, interval);
  }

  void BeginFrameForTesting(base::TimeTicks frame_time) {
    BeginFrame(frame_time);
  }

  void DidSwapBuffersForTesting() {
    DidSwapBuffers();
  }

  int pending_swap_buffers() {
    return pending_swap_buffers_;
  }

  void OnSwapBuffersCompleteForTesting() {
    OnSwapBuffersComplete(NULL);
  }
};

class FakeOutputSurfaceClient : public OutputSurfaceClient {
 public:
  FakeOutputSurfaceClient()
      : begin_frame_count_(0),
        deferred_initialize_result_(true),
        deferred_initialize_called_(false),
        did_lose_output_surface_called_(false) {}

  virtual bool DeferredInitialize(
      scoped_refptr<ContextProvider> offscreen_context_provider) OVERRIDE {
    deferred_initialize_called_ = true;
    return deferred_initialize_result_;
  }
  virtual void SetNeedsRedrawRect(gfx::Rect damage_rect) OVERRIDE {}
  virtual void BeginFrame(base::TimeTicks frame_time) OVERRIDE {
    begin_frame_count_++;
  }
  virtual void OnSwapBuffersComplete(const CompositorFrameAck* ack) OVERRIDE {}
  virtual void DidLoseOutputSurface() OVERRIDE {
    did_lose_output_surface_called_ = true;
  }
  virtual void SetExternalDrawConstraints(const gfx::Transform& transform,
                                          gfx::Rect viewport) OVERRIDE {}

  int begin_frame_count() {
    return begin_frame_count_;
  }

  void set_deferred_initialize_result(bool result) {
    deferred_initialize_result_ = result;
  }

  bool deferred_initialize_called() {
    return deferred_initialize_called_;
  }

  bool did_lose_output_surface_called() {
    return did_lose_output_surface_called_;
  }

 private:
  int begin_frame_count_;
  bool deferred_initialize_result_;
  bool deferred_initialize_called_;
  bool did_lose_output_surface_called_;
};

TEST(OutputSurfaceTest, ClientPointerIndicatesBindToClientSuccess) {
  scoped_ptr<TestWebGraphicsContext3D> context3d =
      TestWebGraphicsContext3D::Create();

  TestOutputSurface output_surface(
      context3d.PassAs<WebKit::WebGraphicsContext3D>());
  EXPECT_FALSE(output_surface.HasClientForTesting());

  FakeOutputSurfaceClient client;
  EXPECT_TRUE(output_surface.BindToClient(&client));
  EXPECT_TRUE(output_surface.HasClientForTesting());
  EXPECT_FALSE(client.deferred_initialize_called());

  // Verify DidLoseOutputSurface callback is hooked up correctly.
  EXPECT_FALSE(client.did_lose_output_surface_called());
  output_surface.context3d()->loseContextCHROMIUM(
      GL_GUILTY_CONTEXT_RESET_ARB, GL_INNOCENT_CONTEXT_RESET_ARB);
  EXPECT_TRUE(client.did_lose_output_surface_called());
}

TEST(OutputSurfaceTest, ClientPointerIndicatesBindToClientFailure) {
  scoped_ptr<TestWebGraphicsContext3D> context3d =
      TestWebGraphicsContext3D::Create();

  // Lose the context so BindToClient fails.
  context3d->set_times_make_current_succeeds(0);

  TestOutputSurface output_surface(
      context3d.PassAs<WebKit::WebGraphicsContext3D>());
  EXPECT_FALSE(output_surface.HasClientForTesting());

  FakeOutputSurfaceClient client;
  EXPECT_FALSE(output_surface.BindToClient(&client));
  EXPECT_FALSE(output_surface.HasClientForTesting());
}

class InitializeNewContext3D : public ::testing::Test {
 public:
  InitializeNewContext3D()
      : context3d_(TestWebGraphicsContext3D::Create()),
        output_surface_(
            scoped_ptr<SoftwareOutputDevice>(new SoftwareOutputDevice)) {}

 protected:
  void BindOutputSurface() {
    EXPECT_TRUE(output_surface_.BindToClient(&client_));
    EXPECT_TRUE(output_surface_.HasClientForTesting());
  }

  void InitializeNewContextExpectFail() {
    EXPECT_FALSE(output_surface_.InitializeNewContext3D(
        context3d_.PassAs<WebKit::WebGraphicsContext3D>()));
    EXPECT_TRUE(output_surface_.HasClientForTesting());

    EXPECT_FALSE(output_surface_.context3d());
    EXPECT_TRUE(output_surface_.software_device());
  }

  scoped_ptr<TestWebGraphicsContext3D> context3d_;
  TestOutputSurface output_surface_;
  FakeOutputSurfaceClient client_;
};

TEST_F(InitializeNewContext3D, Success) {
  BindOutputSurface();
  EXPECT_FALSE(client_.deferred_initialize_called());

  EXPECT_TRUE(output_surface_.InitializeNewContext3D(
      context3d_.PassAs<WebKit::WebGraphicsContext3D>()));
  EXPECT_TRUE(client_.deferred_initialize_called());

  EXPECT_FALSE(client_.did_lose_output_surface_called());
  output_surface_.context3d()->loseContextCHROMIUM(
      GL_GUILTY_CONTEXT_RESET_ARB, GL_INNOCENT_CONTEXT_RESET_ARB);
  EXPECT_TRUE(client_.did_lose_output_surface_called());
}

TEST_F(InitializeNewContext3D, Context3dMakeCurrentFails) {
  BindOutputSurface();
  context3d_->set_times_make_current_succeeds(0);
  InitializeNewContextExpectFail();
}

TEST_F(InitializeNewContext3D, ClientDeferredInitializeFails) {
  BindOutputSurface();
  client_.set_deferred_initialize_result(false);
  InitializeNewContextExpectFail();
}

TEST(OutputSurfaceTest, BeginFrameEmulation) {
  scoped_ptr<TestWebGraphicsContext3D> context3d =
      TestWebGraphicsContext3D::Create();

  TestOutputSurface output_surface(
      context3d.PassAs<WebKit::WebGraphicsContext3D>());
  EXPECT_FALSE(output_surface.HasClientForTesting());

  FakeOutputSurfaceClient client;
  EXPECT_TRUE(output_surface.BindToClient(&client));
  EXPECT_TRUE(output_surface.HasClientForTesting());
  EXPECT_FALSE(client.deferred_initialize_called());

  // Initialize BeginFrame emulation
  scoped_refptr<base::TestSimpleTaskRunner> task_runner =
      new base::TestSimpleTaskRunner;
  bool throttle_frame_production = true;
  const base::TimeDelta display_refresh_interval =
      base::TimeDelta::FromMicroseconds(16666);

  output_surface.InitializeBeginFrameEmulation(
      task_runner.get(),
      throttle_frame_production,
      display_refresh_interval);

  output_surface.SetMaxFramesPending(2);

  // We should start off with 0 BeginFrames
  EXPECT_EQ(client.begin_frame_count(), 0);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 0);

  // We should not have a pending task until a BeginFrame has been requested.
  EXPECT_FALSE(task_runner->HasPendingTask());
  output_surface.SetNeedsBeginFrame(true);
  EXPECT_TRUE(task_runner->HasPendingTask());

  // BeginFrame should be called on the first tick.
  task_runner->RunPendingTasks();
  EXPECT_EQ(client.begin_frame_count(), 1);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 0);

  // BeginFrame should not be called when there is a pending BeginFrame.
  task_runner->RunPendingTasks();
  EXPECT_EQ(client.begin_frame_count(), 1);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 0);

  // DidSwapBuffers should clear the pending BeginFrame.
  output_surface.DidSwapBuffersForTesting();
  EXPECT_EQ(client.begin_frame_count(), 1);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 1);
  task_runner->RunPendingTasks();
  EXPECT_EQ(client.begin_frame_count(), 2);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 1);

  // BeginFrame should be throttled by pending swap buffers.
  output_surface.DidSwapBuffersForTesting();
  EXPECT_EQ(client.begin_frame_count(), 2);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 2);
  task_runner->RunPendingTasks();
  EXPECT_EQ(client.begin_frame_count(), 2);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 2);

  // SwapAck should decrement pending swap buffers and unblock BeginFrame again.
  output_surface.OnSwapBuffersCompleteForTesting();
  EXPECT_EQ(client.begin_frame_count(), 2);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 1);
  task_runner->RunPendingTasks();
  EXPECT_EQ(client.begin_frame_count(), 3);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 1);

  // Calling SetNeedsBeginFrame again indicates a swap did not occur but
  // the client still wants another BeginFrame.
  output_surface.SetNeedsBeginFrame(true);
  task_runner->RunPendingTasks();
  EXPECT_EQ(client.begin_frame_count(), 4);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 1);

  // Disabling SetNeedsBeginFrame should prevent further BeginFrames.
  output_surface.SetNeedsBeginFrame(false);
  task_runner->RunPendingTasks();
  EXPECT_FALSE(task_runner->HasPendingTask());
  EXPECT_EQ(client.begin_frame_count(), 4);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 1);

  // Optimistically injected BeginFrames without a SetNeedsBeginFrame should be
  // allowed.
  output_surface.BeginFrameForTesting(base::TimeTicks::Now());
  EXPECT_EQ(client.begin_frame_count(), 5);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 1);

  // Optimistically injected BeginFrames without a SetNeedsBeginFrame should
  // still be throttled by pending begin frames however.
  output_surface.BeginFrameForTesting(base::TimeTicks::Now());
  EXPECT_EQ(client.begin_frame_count(), 5);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 1);

  // Optimistically injected BeginFrames without a SetNeedsBeginFrame should
  // also be throttled by pending swap buffers.
  output_surface.DidSwapBuffersForTesting();
  EXPECT_EQ(client.begin_frame_count(), 5);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 2);
  output_surface.BeginFrameForTesting(base::TimeTicks::Now());
  EXPECT_EQ(client.begin_frame_count(), 5);
  EXPECT_EQ(output_surface.pending_swap_buffers(), 2);
}

}  // namespace
}  // namespace cc
