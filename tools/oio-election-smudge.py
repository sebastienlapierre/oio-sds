#!/usr/bin/env python

# oio-election-smudge.py, a CLI tool of OpenIO SDS, a tool you SHOULD NOT USE
# Copyright (C) 2016 OpenIO, original work as part of OpenIO SDS
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

import sys
import signal
import logging
import itertools
import zookeeper
from oio.common.utils import load_namespace_conf

PREFIX='/hc'
PREFIX_NS=PREFIX+'/ns'
hexa = '0123456789ABCDEF'
SRVTYPES = {
        "meta0": (0,0),
        "meta1": (1,3),
        "meta2": (2,2),
        "sqlx":  (2,2),
}
acl_openbar = [{'perms': zookeeper.PERM_ALL,
                'scheme': 'world',
                'id': 'anyone'}]

def hash_tokens (w):
    if w == 0:
        return []
    return itertools.product(hexa, repeat=w)

def hash_tree (d0, w):
    tokens = [''.join(x) for x in hash_tokens(w)]

    def depth(d):
        if d == 0:
            return []
        return itertools.product(tokens, repeat=d)
    for x in depth(d0):
        yield '/'.join(x)

def namespace_tree (ns, srvtype):
    d, w = SRVTYPES[srvtype]
    basedir = PREFIX_NS+'/'+ns+'/el/'+srvtype
    for x in hash_tree(d, w):
        yield basedir+'/'+x

def list_groups(zh, path, options):
    path = path.replace('//', '/')
    try:
        children = list(zookeeper.get_children(zh, path))
        if len(children) <= 0:
            return
        seen = dict()
        for child in children:
            key, num = child.split('-', 1)
            if key not in seen:
                seen[key] = []
            seen[key].append(num)
        for k, nums in seen.iteritems():
            nums = sorted(nums)
            yield path, k, nums[0], nums[-1]

    except Exception as e:
        logging.warn("ERROR list %s: %s", path, e)
        raise

def create_node (zh, options, path):
    try:
        zookeeper.create(zh, path, options.VALUE,
                         acl_openbar, zookeeper.EPHEMERAL)
        logging.info("OK create %s", path)
    except Exception as e:
        logging.info("ERROR create %s: %s", path, e)

def main():
    from optparse import OptionParser as OptionParser

    parser = OptionParser()
    parser.add_option(
            '-v', '--verbose',
            action="store_true", dest="flag_verbose",
            help='Triggers debugging traces')
    parser.add_option(
            '-d', '--data',
            action="store", dest="VALUE", default='',
            help="Data to force")

    (options, args) = parser.parse_args(sys.argv)

    # Logging configuration
    if options.flag_verbose:
        logging.basicConfig(
            format='%(asctime)s %(message)s',
            datefmt='%m/%d/%Y %I:%M:%S',
            level=logging.DEBUG)
    else:
        logging.basicConfig(
            format='%(asctime)s %(message)s',
            datefmt='%m/%d/%Y %I:%M:%S',
            level=logging.INFO)

    if len(args) < 2:
        raise ValueError("not enough CLI arguments: NS TYPE [TYPE...]")

    ns = args[1]
    cnxstr = load_namespace_conf(ns)['zookeeper']

    zookeeper.set_debug_level(zookeeper.LOG_LEVEL_INFO)
    zh = zookeeper.init(cnxstr)

    for srvtype in args[2:]:
        for group in namespace_tree(ns, srvtype):
            logging.debug(">DIR %s", group)
            for mom, key, first, last in list_groups(zh, group, options):
                tail = str(1+int(last)).rjust(10, '0')
                path = mom + '/' + key + '-' + tail
                create_node(zh, options, path)

    logging.debug("Please send a signal to remove the ephemeral nodes")
    logging.warn("PAUSED ...")
    signal.pause()
    zookeeper.close(zh)

if __name__ == '__main__':
    main()
