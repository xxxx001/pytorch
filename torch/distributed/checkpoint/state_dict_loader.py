import os
import warnings
from typing import Any, Dict, Optional, Union

import torch
import torch.distributed as dist
from torch.distributed.checkpoint.stateful import Stateful

from .default_planner import DefaultLoadPlanner
from .filesystem import FileSystemReader
from .planner import LoadPlanner
from .storage import StorageReader
from .utils import _all_gather_keys, _api_bc_check, _DistWrapper, _profile

__all__ = ["load_state_dict", "load"]


def load_state_dict(
    state_dict: Dict[str, Any],
    storage_reader: StorageReader,
    process_group: Optional[dist.ProcessGroup] = None,
    coordinator_rank: int = 0,
    no_dist: bool = False,
    planner: Optional[LoadPlanner] = None,
) -> None:
    """This method is deprecated. Please switch to 'load'."""
    warnings.warn(
        "'load_state_dict' is deprecated and will be removed in future versions. "
        "Please use 'load' instead."
    )
    storage_reader.reset()
    with _profile():
        # TODO: test returning `load` here instead.
        return _load_state_dict(
            state_dict,
            storage_reader,
            process_group,
            coordinator_rank,
            no_dist,
            planner,
        )


@_api_bc_check
def load(
    state_dict: Dict[str, Any],
    *,
    checkpoint_id: Union[str, os.PathLike, None] = None,
    storage_reader: Optional[StorageReader] = None,
    planner: Optional[LoadPlanner] = None,
    process_group: Optional[dist.ProcessGroup] = None,
    coordinator_rank: int = 0,
    no_dist: bool = False,
) -> None:
    """
    Load a distributed ``state_dict`` in SPMD style.

    Each rank will try to read the least amount of data necessary
    to fullfill the requested `state_dict`. When loading :class:`ShardedTensor`
    or :class:`DTensor` instances, each rank only reads data for their local shards.

    For each ``Stateful`` object (having both a ``state_dict`` and a ``load_state_dict``),
    load will first call ``state_dict`` before attempting deserialization, followed by
    ``load_state_dict`` once the deserialization is complete.

    .. warning::
        All tensors in ``state_dict`` must be allocated on their
        destination device *prior to* calling this function.

        All non-tensor data is loaded using `torch.load()` and modified in place
        on state_dict.

    .. warning::
        Users must call `load_state_dict` on the root module to ensure load
        pos-processing and non-tensor data properly propagates.

    .. note:
        This function can be used for local inference and load a checkpoint
        produced by ``save_state_dict`` without having a process group initialized
        by passing ``no_dist=True`` and by using Tensors instead of ShardedTensors.

    Args:
        state_dict (Dict[str, Any]): The state_dict to save.
        checkpoint_id (Union[str, os.PathLike, None]):
            The ID of this checkpoint instance. The meaning of the checkpoint_id
            depends on the storage. It can be a path to a folder or to a file.
            It can also be a key if the storage is a key-value store.
            (Default: ``None``)
        storage_reader (Optional[StorageReader]):
            Instance of StorageWriter used to perform reads. If this is not
            specified, DCP will automatically infer the reader based on the
            checkpoint_id. If checkpoint_id is also None, an exception will
            be raised. (Default: ``None``)
        planner (Optional[LoadPlanner]):
            Instance of LoadPlanner. If this is not specificed, the default
            planner will be used. (Default: ``None``)
        process_group (Optional[ProcessGroup]):
            ProcessGroup to be used for cross-rank synchronization.
            (Default: ``None``)
        coordinator_rank (int): Rank to use to coordinate the checkpoint.
            rank0 is used by default. (Default: ``0``)
        no_dist (bool): If ``True``, distributed checkpoint will not save
            in SPMD style. (Default: ``False``)

    Returns:
        None.

    Examples
        >>> # xdoctest: +SKIP
        >>> my_model = MyModule()
        >>> optimizer = Adagrad(my_model.parameters())
        >>> model_state_dict = my_model.state_dict()
        >>> fs_storage_reader = torch.distributed.checkpoint.FileSystemReader("/checkpoint/1")

        >>> torch.distributed.checkpoint.load_state_dict(
        >>>     state_dict=model_state_dict,
        >>>     storage_reader=fs_storage_reader,
        >>> )

        >>> # module.load_state_dict() function might have customized steps
        >>> # to flush the state_dict, must call it to
        >>> # ensure correct behavior.
        >>> my_model.load_state_dict(model_state_dict)

    .. note::
        load_state_dict uses collectives to coordinate reads across ranks.
        For NCCL-based process groups, internal tensor representations of
        objects must be moved to the GPU device before communication takes place.
        In this case, the device used is given by ``torch.cuda.current_device()``
        and it is the user's responsibility to ensure that this is set so that each
        rank has an individual GPU, via ``torch.cuda.set_device()``.
    """

    with _profile():
        if not storage_reader:
            if not checkpoint_id:
                raise RuntimeError(
                    "`checkpoint_id` must be specificed if storage_reader is None."
                )
            # TODO: automatically decide whether to use FSSpecFileSystem
            # https://github.com/pytorch/pytorch/issues/118033 and
            # https://github.com/pytorch/pytorch/issues/118036
            storage_reader = FileSystemReader(checkpoint_id)

        storage_reader.reset(checkpoint_id)

        if no_dist:
            keys = list(state_dict.keys())
        else:
            keys = _all_gather_keys(state_dict, process_group)
            if keys != sorted(state_dict.keys()):
                warnings.warn(
                    "Detected mismatched keys in state dict after all gather!"
                    " This behavior is unsupported and may cause errors may cause errors."
                )

        statetful_sd = {}
        for key in keys:
            if key not in state_dict:
                continue
            elem = state_dict[key]
            statetful_sd[key] = (
                elem.state_dict() if isinstance(elem, Stateful) else elem
            )

        _load_state_dict(
            statetful_sd,
            storage_reader,
            process_group,
            coordinator_rank,
            no_dist,
            planner,
        )
        for key in keys:
            if key not in state_dict:
                continue
            elem = state_dict[key]
            if isinstance(elem, Stateful):
                elem.load_state_dict(statetful_sd[key])
            state_dict[key] = elem


def _load_state_dict(
    state_dict: Dict[str, Any],
    storage_reader: StorageReader,
    process_group: Optional[dist.ProcessGroup] = None,
    coordinator_rank: int = 0,
    no_dist: bool = False,
    planner: Optional[LoadPlanner] = None,
) -> None:
    torch._C._log_api_usage_once("torch.distributed.checkpoint.load_state_dict")

    distW = _DistWrapper(process_group, not no_dist, coordinator_rank)
    if planner is None:
        planner = DefaultLoadPlanner()

    def local_step():
        assert planner is not None
        metadata = storage_reader.read_metadata()
        planner.set_up_planner(state_dict, metadata, distW.is_coordinator)
        storage_reader.set_up_storage_reader(metadata, distW.is_coordinator)

        local_plan = planner.create_local_plan()
        local_plan = storage_reader.prepare_local_plan(local_plan)
        return local_plan

    def global_step(all_local_plans):
        assert planner is not None
        all_local_plans = planner.create_global_plan(all_local_plans)
        all_local_plans = storage_reader.prepare_global_plan(all_local_plans)
        return all_local_plans

    central_plan = distW.reduce_scatter("plan", local_step, global_step)

    def read_data():
        assert planner is not None
        final_local_plan = planner.finish_plan(central_plan)
        all_reads = storage_reader.read_data(final_local_plan, planner)

        all_reads.wait()
        return None

    _ = distW.all_gather("read", read_data)
