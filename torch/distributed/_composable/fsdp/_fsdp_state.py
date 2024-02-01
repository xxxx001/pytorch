from typing import Any, Dict, List, Optional, Tuple

import torch
import torch.nn as nn

from torch.distributed._composable_state import (
    _get_module_state,
    _insert_module_state,
    _State,
)
from torch.distributed.utils import _to_kwargs
from torch.utils.hooks import RemovableHandle
from ._fsdp_common import TrainingState
from ._fsdp_param import FSDPParam

from ._fsdp_param_group import FSDPParamGroup


class FSDPState(_State):
    _module: nn.Module  # permit ref cycle since module and state lifetimes are 1:1
    _device: torch.device

    def __init__(self):
        super().__init__()
        self._fsdp_param_group: Optional[FSDPParamGroup] = None
        self._is_root: Optional[bool] = None
        self._training_state: TrainingState = TrainingState.IDLE
        self._pre_forward_hook_handle: Optional[RemovableHandle] = None

        # Attributes only used on the root state:
        self._all_states: List[FSDPState] = []

    # Define a separate init since `__init__` is called in the contract
    def init(self, module: nn.Module, device: torch.device) -> None:
        _insert_module_state(module, self)
        self._module = module
        self._device = device
        self._pre_forward_hook_handle = self._module.register_forward_pre_hook(
            self._pre_forward, prepend=True, with_kwargs=True
        )

    def _root_pre_forward(
        self, module: nn.Module, args: Tuple[Any, ...], kwargs: Dict[str, Any]
    ) -> Tuple[Tuple[Any, ...], Dict[str, Any]]:
        self._lazy_init()
        if not self._is_root:
            return args, kwargs
        with torch.profiler.record_function("FSDP::root_pre_forward"):
            if self._device.type == "cuda":
                with torch.profiler.record_function("FSDP::inputs_to_device"):
                    args_tuple, kwargs_tuple = _to_kwargs(
                        args, kwargs, self._device, False
                    )  # same as DDP
                args, kwargs = args_tuple[0], kwargs_tuple[0]
        return args, kwargs

    def _lazy_init(self) -> None:
        if self._is_root is not None:
            return  # no-op: already initialized
        self._is_root = True
        root_module = self._module
        for module in root_module.modules():
            if (state := _get_module_fsdp_state(module)) is not None:
                if module is not root_module:
                    state._is_root = False
                self._all_states.append(state)
        self._init_fqns()

    def _init_fqns(self) -> None:
        """Sets module and parameter FQN attributes for debugging."""
        assert self._is_root
        root_module = self._module
        param_to_fsdp_param: Dict[nn.Parameter, FSDPParam] = {}
        module_to_fsdp_param_group: Dict[nn.Module, FSDPParamGroup] = {}
        for state in self._all_states:
            if fsdp_param_group := state._fsdp_param_group:
                for fsdp_param in fsdp_param_group.fsdp_params:
                    param_to_fsdp_param[fsdp_param.sharded_param] = fsdp_param
                module_to_fsdp_param_group[fsdp_param_group.module] = fsdp_param_group
        for param_name, param in root_module.named_parameters():
            if param in param_to_fsdp_param:
                param_to_fsdp_param[param]._param_fqn = param_name
        for module_name, module in root_module.named_modules():
            if module in module_to_fsdp_param_group:
                module_to_fsdp_param_group[module]._module_fqn = module_name

    def _pre_forward(
        self, module: nn.Module, args: Tuple[Any, ...], kwargs: Dict[str, Any]
    ) -> Tuple[Tuple[Any, ...], Dict[str, Any]]:
        self._training_state = TrainingState.FORWARD
        args, kwargs = self._root_pre_forward(module, args, kwargs)
        return args, kwargs


def _get_module_fsdp_state(module: nn.Module) -> Optional[FSDPState]:
    state = _get_module_state(module)
    if isinstance(state, FSDPState):
        return state
    return None
