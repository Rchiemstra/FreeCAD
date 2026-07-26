# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from Base.Metadata import export, constmethod
from Base.BaseClass import BaseClass
from typing import Any, Final, overload, Dict


@export(
    Constructor=True,
    Reference=True,
)
class StringHasher(BaseClass):
    """
    This is the StringHasher class

    Author: Zheng, Lei (realthunder.dev@gmail.com)
    Licence: LGPL
    """

    @overload
    def getID(self, txt: str, base64: bool = False, hashable: bool = False, /) -> Any:
        ...

    @overload
    def getID(self, id: int, index: int = 0, /) -> Any:
        ...

    def getID(self, arg: Any, base64: bool = False, hashable: bool = False, /) -> Any:
        """
        If the input is text, return a StringID object that is unique within this hasher. This
        StringID object is reference counted. The hasher may only save hash ID's that are used.

        If the input is an integer, then the hasher will try to find the StringID object stored
        with the same integer value.

        base64=False, hashable=False: store plaintext (existing default text behaviour).
        base64=False, hashable=True: Option::Hashable (long text above Threshold becomes IsHashed).
        base64=True, hashable=False: Option::Binary only; decoded bytes keep full length/NULs;
                                     default is IsBinary and not IsHashed.
        base64=True, hashable=True: Option::Binary | Option::Hashable.
        Malformed base64 raises ValueError (AbortOnBase64DecodingErrors).
        """
        ...

    @constmethod
    def isSame(self, other: "StringHasher", /) -> bool:
        """
        Check if two hasher are the same
        """
        ...

    Count: Final[int] = 0
    """Return count of used hashes"""

    Size: Final[int] = 0
    """Return the size of the hashes"""

    SaveAll: bool = False
    """Whether to save all string hashes regardless of its use count"""

    Threshold: int = 0
    """Data length exceed this threshold will be hashed before storing"""

    Table: Final[Dict[int, str]] = {}
    """Return the entire string table as Int->String dictionary"""
