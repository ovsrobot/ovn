import getopt
import os
import re
import sys
import uuid

import ovs.db.idl
import ovs.db.schema
import ovs.db.types
import ovs.ovsuuid
import ovs.poller
import ovs.stream
import ovs.util
import ovs.vlog
from ovs.db import data
from ovs.db import error
from ovs.db.idl import _row_to_uuid as row_to_uuid
from ovs.fatal_signal import signal_alarm

vlog = ovs.vlog.Vlog("template-lb-stress")
vlog.set_levels_from_string("console:info")
vlog.init(None)

SCHEMA = '../ovn-nb.ovsschema'


def add_chassis_template_vars(idl, n, n_vips, n_backends):
    for i in range(1, n + 1):
        print(f'ADDING LBs for node {i}')
        txn = ovs.db.idl.Transaction(idl)
        tv = txn.insert(idl.tables["Chassis_Template_Var"])
        tv.chassis = f'chassis-{i}'
        tv.setkey('variables', 'vip', f'42.42.42.{i}')

        for j in range(1, n_vips + 1):
            backends = ''
            for k in range(0, n_backends):
                j1 = j // 250
                j2 = j % 250
                backends = f'42.{k}.{j1}.{j2}:{j},{backends}'
            tv.setkey('variables', f'backends{j}', backends)
        status = txn.commit_block()
        sys.stdout.write(
            f'commit status = {ovs.db.idl.Transaction.status_to_string(status)}\n'
        )


def run(remote, n, n_vips, n_backends):
    schema_helper = ovs.db.idl.SchemaHelper(SCHEMA)
    schema_helper.register_all()
    idl = ovs.db.idl.Idl(remote, schema_helper, leader_only=False)

    seqno = 0

    error, stream = ovs.stream.Stream.open_block(
        ovs.stream.Stream.open(remote), 2000
    )
    if error:
        sys.stderr.write(f'failed to connect to \"{remote}\"')
        sys.exit(1)

    if not stream:
        sys.stderr.write(f'failed to connect to \"{remote}\"')
        sys.exit(1)
    rpc = ovs.jsonrpc.Connection(stream)

    while idl.change_seqno == seqno and not idl.run():
        rpc.run()

        poller = ovs.poller.Poller()
        idl.wait(poller)
        rpc.wait(poller)
        poller.block()

    add_chassis_template_vars(idl, n, n_vips, n_backends)


def main(argv):
    try:
        options, args = getopt.gnu_getopt(
            argv[1:], 'n:v:b:r:', ['vips', 'backends', 'remote']
        )
    except getopt.GetoptError as geo:
        sys.stderr.write(f'{ovs.util.PROGRAM_NAME}: {geo.msg}\n')
        sys.exit(1)

    n = None
    vips = None
    backends = None
    remote = None
    for key, value in options:
        if key == '-n':
            n = int(value)
        elif key in ['-v', '--vips']:
            vips = int(value)
        elif key in ['-b', '--backends']:
            backends = int(value)
        elif key in ['-r', '--remote']:
            remote = value
        else:
            sys.stderr.write(f'{ovs.util.PROGRAM_NAME}: unknown input args')
            sys.exit(1)

    if not n or not vips or not backends:
        sys.stderr.write(f'{ovs.util.PROGRAM_NAME}: invalid input args')
        sys.exit(1)

    run(remote, n, vips, backends)


if __name__ == '__main__':
    try:
        main(sys.argv)
    except error.Error as e:
        sys.stderr.write(f'{e}\n')
        sys.exit(1)
