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


def ceil_div(numerator: int, denominator: int) -> int:
    result = int(numerator / denominator)

    if numerator % denominator != 0:
        result += 1

    return result


class TimingBmt:
    def __init__(
        self,
        arity: int,
        total_protected_data: int,
    ) -> None:
        self.arity = arity
        self.total_protected_data = total_protected_data

        self.data_size = 0

        # Note that this should be in sync with the C++ version

        self.mac_count = ceil_div(
            total_protected_data, self._get_hash_input_size()
        )
        # print(f"PYTHON: Number of MACs: {self.mac_count}")
        self.mac_nodes = ceil_div(self.mac_count, arity)
        # print(f"PYTHON: Number of MAC nodes: {self.mac_nodes}")
        self.data_size += self.mac_nodes

        self.counter_count = ceil_div(
            total_protected_data, self._get_hash_input_size()
        )
        # print(f"PYTHON: Number of (minor) counters: {self.counter_count}")

        minor_counter_space = (
            self._get_block_size_bytes() * 8
        ) - self._get_major_counter_bits()
        counters_per_page = int(
            self._get_page_size_bytes() / self._get_hash_input_size()
        )
        self.minor_counter_bits = int(minor_counter_space / counters_per_page)
        # print(
        #     f"PYTHON: Major counter size: {self._get_major_counter_bits()} bits"
        # )
        # print(f"PYTHON: Minor counter size: {self.minor_counter_bits} bits")

        assert (
            self._get_major_counter_bits()
            + (self.minor_counter_bits * counters_per_page)
        ) <= (self._get_block_size_bytes() * 8)

        self.counter_nodes = ceil_div(self.counter_count, counters_per_page)
        # print(f"PYTHON: Number of counter nodes: {self.counter_nodes}")
        self.data_size += self.counter_nodes

        self.leaves = ceil_div(self.counter_nodes, arity)
        self.height = ceil(log(self.leaves, arity)) + 1

        # print(f"PYTHON: The height of the tree is {self.height}")

        non_leaf_nodes = int(
            (self.arity ** (self.height - 1)) / (self.arity - 1)
        )
        self.data_size += non_leaf_nodes
        self.data_size += self.leaves
        self.tree_nodes = non_leaf_nodes + self.leaves

        assert self.tree_nodes > 0
        assert self.counter_nodes > 0
        assert self.mac_nodes > 0
        assert (
            self.data_size
            == self.tree_nodes + self.counter_nodes + self.mac_nodes
        )

        # print(f"PYTHON: Total number of nodes is {self.data_size}")

        # print(
        #     f"PYTHON: Total space protected by tree: {self._stat_data_protected()} (Requested: {total_protected_data} bytes.)"
        # )

        # print(
        #     f"PYTHON: Total space taken by tree nodes: {self.tree_nodes * self._get_block_size_bytes()} bytes."
        # )
        # print(
        #     f"PYTHON: Total space taken by counter nodes: {self.counter_nodes * self._get_block_size_bytes()} bytes."
        # )
        # print(
        #     f"PYTHON: Total space taken by MAC nodes: {self.mac_nodes * self._get_block_size_bytes()} bytes."
        # )
        # print(
        #     f"PYTHON: Total space taken by tree: {self._stat_structure_size()} bytes."
        # )

    def _get_block_size_bytes(self) -> int:
        return 64

    def _get_page_size_bytes(self) -> int:
        return 4096

    def _get_major_counter_bits(self) -> int:
        return 64

    def _get_hash_input_size(self) -> int:
        return self._get_block_size_bytes()

    def _get_hash_output_size(self) -> int:
        return int(self._get_hash_input_size() / self.arity)

    def _stat_data_protected(self) -> int:
        # NOTE Sync with C++

        return int(self.mac_count * self._get_hash_input_size())

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
        #     f"TimingBmt.determine_max_protected_size: min_local_size: {min_local_size}"
        # )
        # print(
        #     f"TimingBmt.determine_max_protected_size: total_local_size: {total_local_size}"
        # )
        # print(
        #     f"TimingBmt.determine_max_protected_size: total_remote_size: {total_remote_size}"
        # )
        # print(
        #     f"TimingBmt.determine_max_protected_size: min_scale_factor: {min_scale_factor}"
        # )
        # print(
        #     f"TimingBmt.determine_max_protected_size: max_scale_factor: {max_scale_factor}"
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
            # print(f"TimingBmt.determine_max_protected_size: ==============")
            tree = TimingBmt(arity, attempted_protected_size)

            # print(
            #     f"TimingBmt.determine_max_protected_size: min_scale_factor: {min_scale_factor} (Local = {min_local_protected}, Remote = {total_remote_size}, Total = {min_local_protected + total_remote_size})"
            # )
            # print(
            #     f"TimingBmt.determine_max_protected_size: selected_scale_factor: {selected_scale_factor} (Local = {attempted_local_protected}, Remote = {total_remote_size}, Total = {attempted_protected_size})"
            # )
            # print(
            #     f"TimingBmt.determine_max_protected_size: --> Available space: {remaining_local_space}"
            # )
            # print(
            #     f"TimingBmt.determine_max_protected_size: --> Tree size: {tree._stat_structure_size()}"
            # )
            # print(
            #     f"TimingBmt.determine_max_protected_size: --> Difference: {remaining_local_space - tree._stat_structure_size()}"
            # )
            # print(
            #     f"TimingBmt.determine_max_protected_size: max_scale_factor: {max_scale_factor} (Local = {max_local_protected}, Remote = {total_remote_size}, Total = {max_local_protected + total_remote_size})"
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
                    f"TimingBmt.determine_max_protected_size: Scale factor found."
                )
                return attempted_local_protected, total_remote_size
            elif tree._stat_structure_size() < remaining_local_space:
                # There is more space that could be protected, since there is leftover space on the table.
                # print(
                #     f"TimingBmt.determine_max_protected_size: Increasing scale factor."
                # )
                min_scale_factor = selected_scale_factor
            else:
                # This structure is too large for the amount of space that is leftover, reduce the protected size to make more space for integrity data
                # print(
                #     f"TimingBmt.determine_max_protected_size: Decreasing scale factor."
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
