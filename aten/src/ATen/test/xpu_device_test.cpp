#include <gtest/gtest.h>

#include <aten/src/ATen/xpu/XPUContext.h>
#include <aten/src/ATen/xpu/XPUDevice.h>
#include <c10/xpu/XPUFunctions.h>

#define ASSERT_EQ_XPU(X, Y) \
  {                         \
    bool _isEQ = X == Y;    \
    ASSERT_TRUE(_isEQ);     \
  }

TEST(XpuDeviceTest, getDeviceProperties) {
  if (!at::xpu::is_available()) {
    return;
  }

  c10::xpu::DeviceProp* cur_device_prop = at::xpu::getCurrentDeviceProperties();
  c10::xpu::DeviceProp* device_prop = at::xpu::getDeviceProperties(0);

  ASSERT_EQ_XPU(cur_device_prop->name, device_prop->name);
  ASSERT_EQ_XPU(cur_device_prop->platform_name, device_prop->platform_name);
  ASSERT_EQ_XPU(cur_device_prop->gpu_eu_count, device_prop->gpu_eu_count);
}

TEST(XpuDeviceTest, getDeviceFromPtr) {
  if (!at::xpu::is_available()) {
    return;
  }

  sycl::device& raw_device = at::xpu::get_raw_device(0);
  void* ptr = sycl::malloc_device(8, raw_device, at::xpu::get_device_context());

  at::Device device = at::xpu::getDeviceFromPtr(ptr);
  sycl::free(ptr, at::xpu::get_device_context());
  ASSERT_EQ_XPU(device.index(), 0);
  ASSERT_EQ_XPU(device.type(), at::kXPU);

  int dummy = 0;
  ASSERT_THROW(at::xpu::getDeviceFromPtr(&dummy), c10::Error);
}

TEST(XpuDeviceTest, getGlobalIdxFromDevice) {
  if (!at::xpu::is_available()) {
    return;
  }

  int target_device = 0;
  auto global_index = at::xpu::getGlobalIdxFromDevice(target_device);
  auto devices = sycl::device::get_devices();
  ASSERT_EQ_XPU(devices[global_index], at::xpu::get_raw_device(target_device));

  void* ptr = sycl::malloc_device(8, devices[global_index], at::xpu::get_device_context());
  at::Device device = at::xpu::getDeviceFromPtr(ptr);
  sycl::free(ptr, at::xpu::get_device_context());
  ASSERT_EQ_XPU(device.index(), target_device);
  ASSERT_EQ_XPU(device.type(), at::kXPU);

  if (at::xpu::device_count() == 1) {
    return;
  }
  // Test the last device.
  target_device = at::xpu::device_count() - 1;
  global_index = at::xpu::getGlobalIdxFromDevice(target_device);
  ASSERT_EQ_XPU(devices[global_index], at::xpu::get_raw_device(target_device));

  target_device = at::xpu::device_count();
  ASSERT_THROW(at::xpu::getGlobalIdxFromDevice(target_device), c10::Error);
}
