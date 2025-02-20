#!/usr/bin/python
"""
  (C) Copyright 2018-2019 Intel Corporation.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
  The Government's rights to use, modify, reproduce, release, perform, display,
  or disclose this software are subject to the terms of the Apache License as
  provided in Contract No. B609815.
  Any reproduction of computer software, computer software documentation, or
  portions thereof marked with this legend must also reproduce the markings.
"""
from ior_test_base import IorTestBase


class IorSingleServer(IorTestBase):
    """Test class Description: Runs IOR with 1 server.

    :avocado: recursive
    """

    def test_singleserver(self):
        """Jira ID: DAOS-XXXX.

        Test Description:
            Test IOR with Single Server config.

        Use Cases:
            Different combinations of 1/64/128 Clients,
            1K/4K/32K/128K/512K/1M transfer size.

        :avocado: tags=ior,singleserver
        """
        self.run_ior_with_pool()
