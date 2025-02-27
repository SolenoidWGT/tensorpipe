/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <vector>

#include <tensorpipe/common/defs.h>
#include <tensorpipe/common/ibv_lib.h>

namespace tensorpipe {

// Error checking macros

#define TP_CHECK_IBV_PTR(op)                   \
  [&]() {                                      \
    auto ptr = op;                             \
    TP_THROW_SYSTEM_IF(ptr == nullptr, errno); \
    return ptr;                                \
  }()

#define TP_CHECK_IBV_INT(op)           \
  {                                    \
    int rv = op;                       \
    TP_THROW_SYSTEM_IF(rv < 0, errno); \
  }

#define TP_CHECK_IBV_VOID(op) op;

// Logging helpers

std::string ibvWorkCompletionOpcodeToStr(IbvLib::wc_opcode opcode);

// RAII wrappers
struct IbvContextDeleter {
  void operator()(IbvLib::context* ptr) {
    TP_CHECK_IBV_INT(ibvLib->close_device(ptr));
  }

  const IbvLib* ibvLib;
};

using IbvContext = std::unique_ptr<IbvLib::context, IbvContextDeleter>;

inline IbvContext createIbvContext(
    const IbvLib& ibvLib,
    IbvLib::device& device) {
  return IbvContext(
      TP_CHECK_IBV_PTR(ibvLib.open_device(&device)),
      IbvContextDeleter{&ibvLib});
}

class IbvDeviceList {
 private:
  IbvDeviceList(
      const IbvLib& ibvLib,
      IbvLib::device** ptr,
      int size,
      std::vector<IbvLib::device*>&& available_deviceList)
      : deviceList_(ptr, Deleter{&ibvLib}),
        size_(size),
        available_deviceList_(available_deviceList) {}

 public:
  IbvDeviceList() = default;

  static std::tuple<Error, IbvDeviceList> create(
      const IbvLib& ibvLib,
      const uint8_t kPortNum = 1) {
    int size;
    std::vector<IbvLib::device*> available_deviceList;
    IbvLib::device** ptr = ibvLib.get_device_list(&size);
    if (ptr == nullptr) {
      // Earlier versions of libibverbs had a bug where errno would be set to
      // *negative* ENOSYS when the module wasn't found. This got fixed in
      // https://github.com/linux-rdma/rdma-core/commit/062bf1a72badaf6ad2d51ebe4c8c8bdccfc376e2
      // However, to support those versions, we manually flip it in case.
      return std::make_tuple(
          TP_CREATE_ERROR(
              SystemError,
              "ibv_get_device_list",
              errno == -ENOSYS ? ENOSYS : errno),
          IbvDeviceList());
    }

    // If the deviceList contains multiple ibv devices, we will select the
    // device of the port whose port_state is active, instead of just selecting
    // the first device in the deviceList by default.
    for (int i = 0; i < size; i++) {
      IbvContext tp_ctx_;
      IbvLib::port_attr portAttr;
      std::memset(&portAttr, 0, sizeof(portAttr));

      try {
        tp_ctx_ = createIbvContext(ibvLib, *ptr[i]);
      } catch (const std::system_error& e) {
        TP_VLOG(1) << "Caught system_error with code " << e.code()
                   << " meaning " << e.what() << "fail to open device "
                   << ptr[i]->name;
        continue;
      }

      TP_CHECK_IBV_INT(ibvLib.query_port(tp_ctx_.get(), kPortNum, &portAttr));

      if (portAttr.link_layer != IbvLib::LINK_LAYER_INFINIBAND &&
          portAttr.link_layer != IbvLib::LINK_LAYER_ETHERNET) {
        TP_VLOG(8)
            << "IbvDevice " << ptr[i]->name << " port " << unsigned(kPortNum)
            << " link_layer is not IBV_LINK_LAYER_INFINIBAND or IBV_LINK_LAYER_ETHERNET"
            << " , skip this device";
        continue;
      }

      if (portAttr.state != IbvLib::port_state::PORT_ACTIVE) {
        TP_VLOG(8) << "IbvDevice " << ptr[i]->name << " port "
                   << unsigned(kPortNum) << " state is "
                   << ibvLib.port_state_str(portAttr.state)
                   << " , skip this device";
        continue;
      }
      available_deviceList.push_back(ptr[i]);
    }

    return std::make_tuple(
        Error::kSuccess,
        IbvDeviceList(
            ibvLib,
            ptr,
            available_deviceList.size(),
            std::move(available_deviceList)));
  }

  int size() {
    return size_;
  }

  IbvLib::device& operator[](int i) {
    return *available_deviceList_[i];
  }

  void reset() {
    available_deviceList_.clear();
    deviceList_.reset();
  }

  // FIXME Can we support a "range" API (i.e., a begin() and end() method) so
  // that this can be used in a for (auto& dev : deviceList) expression?

 private:
  struct Deleter {
    void operator()(IbvLib::device** ptr) {
      TP_CHECK_IBV_VOID(ibvLib->free_device_list(ptr));
    }

    const IbvLib* ibvLib;
  };

  std::unique_ptr<IbvLib::device*, Deleter> deviceList_;
  std::vector<IbvLib::device*> available_deviceList_;
  int size_;
};

struct IbvProtectionDomainDeleter {
  void operator()(IbvLib::pd* ptr) {
    TP_CHECK_IBV_INT(ibvLib->dealloc_pd(ptr));
  }

  const IbvLib* ibvLib;
};

using IbvProtectionDomain =
    std::unique_ptr<IbvLib::pd, IbvProtectionDomainDeleter>;

inline IbvProtectionDomain createIbvProtectionDomain(
    const IbvLib& ibvLib,
    IbvContext& context) {
  return IbvProtectionDomain(
      TP_CHECK_IBV_PTR(ibvLib.alloc_pd(context.get())),
      IbvProtectionDomainDeleter{&ibvLib});
}

struct IbvCompletionQueueDeleter {
  void operator()(IbvLib::cq* ptr) {
    TP_CHECK_IBV_INT(ibvLib->destroy_cq(ptr));
  }

  const IbvLib* ibvLib;
};

using IbvCompletionQueue =
    std::unique_ptr<IbvLib::cq, IbvCompletionQueueDeleter>;

inline IbvCompletionQueue createIbvCompletionQueue(
    const IbvLib& ibvLib,
    IbvContext& context,
    int cqe,
    void* cq_context,
    IbvLib::comp_channel* channel,
    int comp_vector) {
  return IbvCompletionQueue(
      TP_CHECK_IBV_PTR(ibvLib.create_cq(
          context.get(), cqe, cq_context, channel, comp_vector)),
      IbvCompletionQueueDeleter{&ibvLib});
}

struct IbvSharedReceiveQueueDeleter {
  void operator()(IbvLib::srq* ptr) {
    TP_CHECK_IBV_INT(ibvLib->destroy_srq(ptr));
  }

  const IbvLib* ibvLib;
};

using IbvSharedReceiveQueue =
    std::unique_ptr<IbvLib::srq, IbvSharedReceiveQueueDeleter>;

inline IbvSharedReceiveQueue createIbvSharedReceiveQueue(
    const IbvLib& ibvLib,
    IbvProtectionDomain& pd,
    IbvLib::srq_init_attr& initAttr) {
  return IbvSharedReceiveQueue(
      TP_CHECK_IBV_PTR(ibvLib.create_srq(pd.get(), &initAttr)),
      IbvSharedReceiveQueueDeleter{&ibvLib});
}

struct IbvMemoryRegionDeleter {
  void operator()(IbvLib::mr* ptr) {
    TP_CHECK_IBV_INT(ibvLib->dereg_mr(ptr));
  }

  const IbvLib* ibvLib;
};

using IbvMemoryRegion = std::unique_ptr<IbvLib::mr, IbvMemoryRegionDeleter>;

inline IbvMemoryRegion createIbvMemoryRegion(
    const IbvLib& ibvLib,
    IbvProtectionDomain& pd,
    void* addr,
    size_t length,
    int accessFlags) {
  return IbvMemoryRegion(
      TP_CHECK_IBV_PTR(ibvLib.reg_mr(pd.get(), addr, length, accessFlags)),
      IbvMemoryRegionDeleter{&ibvLib});
}

struct IbvQueuePairDeleter {
  void operator()(IbvLib::qp* ptr) {
    TP_CHECK_IBV_INT(ibvLib->destroy_qp(ptr));
  }

  const IbvLib* ibvLib;
};

using IbvQueuePair = std::unique_ptr<IbvLib::qp, IbvQueuePairDeleter>;

inline IbvQueuePair createIbvQueuePair(
    const IbvLib& ibvLib,
    IbvProtectionDomain& pd,
    IbvLib::qp_init_attr& initAttr) {
  return IbvQueuePair(
      TP_CHECK_IBV_PTR(ibvLib.create_qp(pd.get(), &initAttr)),
      IbvQueuePairDeleter{&ibvLib});
}

// Helpers

struct IbvAddress {
  uint8_t portNum;
  uint8_t globalIdentifierIndex;
  // The already-resolved LID of the above device+port pair.
  uint32_t localIdentifier;
  // The already-resolved GID of the above device+port+index combination.
  IbvLib::gid globalIdentifier;
  IbvLib::mtu maximumTransmissionUnit;
  uint32_t maximumMessageSize;
};

struct IbvSetupInformation {
  uint32_t localIdentifier;
  IbvLib::gid globalIdentifier;
  uint32_t queuePairNumber;
  IbvLib::mtu maximumTransmissionUnit;
  uint32_t maximumMessageSize;
};

struct IbvAddress makeIbvAddress(
    const IbvLib& ibvLib,
    const IbvContext& context,
    uint8_t portNum,
    uint8_t globalIdentifierIndex);

struct IbvSetupInformation makeIbvSetupInformation(
    const IbvAddress& addr,
    const IbvQueuePair& qp);

void transitionIbvQueuePairToInit(
    const IbvLib& ibvLib,
    IbvQueuePair& qp,
    const IbvAddress& selfAddr);

void transitionIbvQueuePairToReadyToReceive(
    const IbvLib& ibvLib,
    IbvQueuePair& qp,
    const IbvAddress& selfAddr,
    const IbvSetupInformation& destinationInfo);

void transitionIbvQueuePairToReadyToSend(
    const IbvLib& ibvLib,
    IbvQueuePair& qp);

void transitionIbvQueuePairToError(const IbvLib& ibvLib, IbvQueuePair& qp);

} // namespace tensorpipe
