# Only used to essentially provide the Python version of a C++ function

from math import (
    ceil,
    log,
)
from typing import Tuple


def truncate_to_size_multiple(original_size: int, size: int) -> int:
    # Truncate protected local size to a multiple of a size.
    remainder = original_size % size
    if remainder != 0:
        original_size -= remainder

    return original_size


class TimingTree:
    def __init__(
        self,
        arity: int,
        total_protected_data: int,
    ) -> None:
        self.arity = arity
        self.total_protected_data = total_protected_data

        # Note that this should be in sync with the C++ version

        # Consider the total number of hashes necessary to cover this area.
        leaf_hashes = int(
            self.total_protected_data / self._get_hash_input_size()
        )
        # Handle possible extra remainder.
        if self.total_protected_data % self._get_hash_input_size() != 0:
            leaf_hashes += 1
        # print(f"PYTHON: {leaf_hashes} total leaf hashes needed.")

        last_level_nodes = int(leaf_hashes / self.arity)
        # Handle possible extra remainder.
        if leaf_hashes % self.arity != 0:
            last_level_nodes += 1
        # print(f"PYTHON: {last_level_nodes} last level nodes.")
        self.leaves = last_level_nodes

        # Calculate how many levels you need to get that many leaves in the tree.
        levels = ceil(log(last_level_nodes, self.arity)) + 1
        self.height = levels
        # print(f"PYTHON: The height is {levels}.")

        # Initialize the data size parameter. Take the number of nodes for all the
        # fully-filled levels, then add the number of leaf nodes.
        self.data_size = int((self.arity ** (levels - 1)) / (self.arity - 1))
        self.data_size += last_level_nodes
        # print(f"PYTHON: Total number of nodes is {self.data_size}.")

        # print(
        #     f"PYTHON: Total space protected by tree: {self._stat_data_protected()} bytes. (Requested: {self.total_protected_data})"
        # )

        # print(
        #     f"PYTHON: Total space taken by tree: {self._stat_structure_size()} bytes."
        # )

    def _get_block_size_bytes(self) -> int:
        return 64

    def _get_hash_input_size(self) -> int:
        return self._get_block_size_bytes()

    def _get_hash_output_size(self) -> int:
        return int(self._get_hash_input_size() / self.arity)

    def _stat_data_protected(self) -> int:
        # NOTE Sync with C++

        # Assuming that in the basic version, each leaf contains `arity` number
        # of hashes for real data.

        # Consider the total number of hashes created by the set of leaves.
        leaf_hashes = self.leaves * self.arity

        # Consider the amount of data represented by a single hash.
        return int(leaf_hashes * self._get_hash_input_size())

    def _stat_structure_size(self) -> int:
        # NOTE Sync with C++

        return int(self.data_size * self._get_block_size_bytes())

    @staticmethod
    def determine_max_protected_size(
        min_local_size: int,
        max_local_size: int,
        total_local_size: int,
        total_remote_size: int,
        arity: int,
    ) -> Tuple[int, int]:
        """
        :param min_local_size:    Minimum amount of size that must be protected
                                  in local memory.
        :param max_local_size:    Maximum amount of size that must be protected
                                  in local memory. If this option is `0`, the
                                  theoretical maximum is `total_local_size`.
        :param total_local_size:  Total amount of memory available to work with
                                  in local memory.
        :param total_remote_size: Total amount of memory available to work
                                  with in remote (essentially static
                                  contributor to available space).
        :param arity:             The arity of the tree structure used.

        Returns the amount of data at most (in local, and remote memory) that
        can be protected based on the amount of data it would take to protect
        it, and the total amount of space available.

        Two types of memory: local and remote. ("Local" in this case means
        "the same place as integrity data." "Remote" means "the other
        place that is not the same place as integrity data.")

        This is not an actual bearing on the memory architecture used.

        NOTE: Currently assumes only one type of memory being used to store
              integrity data at a time.
        """

        # Perform a binary search to find the max amount of data to protect

        # Scale factor represents the amount of data to use in local memory.
        if min_local_size != 0:
            min_scale_factor = 1
            if max_local_size != 0:
                max_scale_factor = max_local_size / min_local_size
            else:
                max_scale_factor = total_local_size / min_local_size
        else:
            min_scale_factor = 0
            max_scale_factor = 0
        selected_scale_factor = min_scale_factor + (
            (max_scale_factor - min_scale_factor) / 2
        )
        cache_line_size = 64
        page_size = 4096

        assert total_remote_size % cache_line_size == 0

        # print(
        #     f"TimingTree.determine_max_protected_size: min_local_size: {min_local_size}"
        # )
        # print(
        #     f"TimingTree.determine_max_protected_size: total_local_size: {total_local_size}"
        # )
        # print(
        #     f"TimingTree.determine_max_protected_size: total_remote_size: {total_remote_size}"
        # )
        # print(
        #     f"TimingTree.determine_max_protected_size: min_scale_factor: {min_scale_factor}"
        # )
        # print(
        #     f"TimingTree.determine_max_protected_size: max_scale_factor: {max_scale_factor}"
        # )

        iterations = 0
        while True:
            min_local_protected = truncate_to_size_multiple(
                int(min_local_size * min_scale_factor), cache_line_size
            )
            max_local_protected = truncate_to_size_multiple(
                int(min_local_size * max_scale_factor), cache_line_size
            )

            attempted_local_protected = int(
                min_local_size * selected_scale_factor
            )
            attempted_local_protected = truncate_to_size_multiple(
                attempted_local_protected, cache_line_size
            )

            # Calculate the amount of space we may end up protecting
            attempted_protected_size = (
                total_remote_size + attempted_local_protected
            )

            # (For integrity data)
            remaining_local_space = (
                total_local_size - attempted_local_protected
            )

            # Calculate tree size
            # print(f"TimingTree.determine_max_protected_size: ==============")
            tree = TimingTree(arity, attempted_protected_size)

            # print(
            #     f"TimingTree.determine_max_protected_size: min_scale_factor: {min_scale_factor} (Local = {min_local_protected}, Remote = {total_remote_size}, Total = {min_local_protected + total_remote_size})"
            # )
            # print(
            #     f"TimingTree.determine_max_protected_size: selected_scale_factor: {selected_scale_factor} (Local = {attempted_local_protected}, Remote = {total_remote_size}, Total = {attempted_protected_size})"
            # )
            # print(
            #     f"TimingTree.determine_max_protected_size: --> Available space: {remaining_local_space}"
            # )
            # print(
            #     f"TimingTree.determine_max_protected_size: --> Tree size: {tree._stat_structure_size()}"
            # )
            # print(
            #     f"TimingTree.determine_max_protected_size: --> Difference: {remaining_local_space - tree._stat_structure_size()}"
            # )
            # print(
            #     f"TimingTree.determine_max_protected_size: max_scale_factor: {max_scale_factor} (Local = {max_local_protected}, Remote = {total_remote_size}, Total = {max_local_protected + total_remote_size})"
            # )

            if (
                remaining_local_space - tree._stat_structure_size() >= 0
                and remaining_local_space - tree._stat_structure_size()
                < page_size
            ):
                # We have found the best arrangement of tree size with this amount of protected space. Stop.
                attempted_local_protected = truncate_to_size_multiple(
                    attempted_local_protected, page_size
                )
                print(
                    f"TimingTree.determine_max_protected_size: Scale factor found."
                )
                return attempted_local_protected, total_remote_size
            elif tree._stat_structure_size() < remaining_local_space:
                # There is more space that could be protected, since there is leftover space on the table.
                # print(
                #     f"TimingTree.determine_max_protected_size: Increasing scale factor."
                # )
                min_scale_factor = selected_scale_factor
            else:
                # This structure is too large for the amount of space that is leftover, reduce the protected size to make more space for integrity data
                # print(
                #     f"TimingTree.determine_max_protected_size: Decreasing scale factor."
                # )
                max_scale_factor = selected_scale_factor

            selected_scale_factor = min_scale_factor + (
                (max_scale_factor - min_scale_factor) / 2
            )
            iterations += 1

            if iterations > 500:
                if (
                    remaining_local_space - tree._stat_structure_size()
                    >= page_size
                ):
                    print(
                        "It looks like this configuration is leaving unused space, but this is still a valid configuration."
                    )
                    return attempted_local_protected, total_remote_size

                print(
                    "Could not find a valid size arrangement for this configuration. (The integrity tree may take up too much space no matter what.)"
                )
                exit(1)
