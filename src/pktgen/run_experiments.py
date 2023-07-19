# This program consumes config.json and run
# the epxierments according the configrations.
# Please checkout _config_readme_ for all the possible options.
# 
# The program can be run in the following manner
# Clean the exisiting binary of pktgen
# sudo make clean
# Create new binraies
# sudo make
# Run this program
# sudo python3 run_experiments.py [args]
# The program can take three possible arguments
#   --clean:    remove all the existing results in the results/ directory before running the experiments
#   --cta:      build CTA before running the experiments
#   --pktgen:   build pktgen before running the experiments

from json import load
import subprocess
import os
from os.path import exists
from time import time
from shutil import move, rmtree
from time import sleep
from signal import SIGTERM, SIGINT, signal
from scp import SCPClient
from paramiko import SSHClient, AutoAddPolicy
from string import Template
from argparse import ArgumentParser
from functools import partial

timeout = time() + 60*2  # 2 mins
# ========= CTA related constant parameters ===========
cta_log_files = ["CTA_console_logs.txt"]

# ========= CPF related constant parameters ===========
cpf_log_files = ["CPF_console_logs.txt", "CPF_logs.txt"]

# ========= Common parameters =========================
cta_args = Template("$scheme $cpfs $remote_cpfs $replicas $remote_replicas $tx_arg $delay $procedure $cpfs_action")
cpf_args = Template("$scheme $cpfs $remote_cpfs $replicas $remote_replicas $tx_arg $delay $procedure $cpfs_action")

# ========= Pktgen related constant parameters ===========
pktgen_log_files = ["pktgen_stats.txt", "pktgen_console_logs.txt", "PCT.txt"]
pktgen_conf = "config.json"
servers_credentials_conf = "servers_credentials.json"
pktgen_args = Template(
    "-d $duration -r $rate -u $workers -s $scheme -p $procedure -c $proc_count")
pktgen_desc = Template(
    "Running exp $id of $scheme $proc in $mode at rate = $rate | delay = $delay | tx_arg = $tx_arg | remote_replicas = $remote_replicas | local_replicas = $local_replicas")
cta_conn = None
cpf_conn = None

def arg_parser():
    parser = ArgumentParser(description="Fast-CTA experiment scripts")
    parser.add_argument("--clean", type=bool, default=False,
                        nargs="?", help="clean logs", const=True)
    parser.add_argument("--cta", type=bool, default=False,
                        nargs="?", help="build CTA before", const=True)
    parser.add_argument("--pktgen", type=bool, default=False,
                        nargs="?", help="build Pktgen before", const=True)
    return parser.parse_args()


def read_if_possible(chan):
    while chan.recv_ready():
        chan.recv(4096)


def run_cmd(cmd):
    subprocess.Popen([cmd], shell=True, stdout=None,
                     stderr=None, close_fds=True, preexec_fn=os.setsid)


def make_res_dir(kv, pktgen_credentials):

    pktgen_root = pktgen_credentials['root_dir']
    pktgen_res_dir = pktgen_root + "results/"

    results_dname = Template(pktgen_res_dir + "$mode-$scheme-$proc/$rate/$itr/")

    mode = "b" if kv["bursty"] else "u"

    path = results_dname.substitute({"scheme":  kv["scheme"],
                                     "proc":    kv["procedure"],
                                     "itr":     kv["exp_id"],
                                     "rate":    kv["curr_rate"],
                                     "mode":    mode, })
    if not os.path.exists(path):
        os.makedirs(path)
    print(path)
    return path


def parse_cta_args(conf):
    return cta_args.substitute(conf)

def parse_cpf_args(conf):
    return cpf_args.substitute(conf)


def parse_pktgen_args(conf):
    return pktgen_args.substitute(conf)


def load_config(f_name):
    with open(f_name, 'r') as f:
        configs = load(f)
    return configs


def connect_entity(ip, username, self_key):
    ssh = SSHClient()
    ssh.load_system_host_keys()
    ssh.set_missing_host_key_policy(AutoAddPolicy())

    ssh.connect(ip, 22, username=username, key_filename=self_key)
    
    chan = ssh.get_transport().open_session()
    chan.invoke_shell()
    read_if_possible(chan)
    return ssh, chan


def build_cta(chan):
    chan.sendall("cd {}\n".format(cta_root))
    chan.sendall("sudo make\n")
    read_if_possible(chan)


def run_core(conn, chan, root, logs_dir, entity, py_cmd, entity_credentials):
    
    global timeout
    chan.sendall("cd {}\n".format(root))
    chan.sendall("mkdir -p {}\n".format(logs_dir))
    temp = "sudo python3 run.py {}\n".format(py_cmd)
    stdin, stdout, stderr = conn.exec_command("cd {}; sudo python3 run.py {}\n".format(root, py_cmd), get_pty=True)
    stdin.write(entity_credentials['password'] + '\n')
    read_if_possible(chan)

    core_lck = "{}core.lck".format(root)
    while True:
        _, stdout, _ = conn.exec_command("ls {}".format(core_lck))
        status = stdout.readlines()
        if len(status) > 0 or time() > timeout:
            timeout = time() + 60*2
            break


def stop_core(entity, conn, root, server_credentials):
    stdin, stdout, stderr = conn.exec_command(
        "sudo pkill --signal SIGINT {}".format(entity), get_pty=True)

    stdin.write(server_credentials['password'] + '\n')

    stdin.flush()
    stdout.channel.recv_exit_status()
    stderr.channel.recv_exit_status()

    while True:
        _, stdout, _ = conn.exec_command("find {} -name core.lck".format(root), get_pty=True)
        status = stdout.readlines()
        if len(status) == 0:
            break



def build_pktgen():
    subprocess.call(["make clean && make"])


def run_pktgen(pktgen_cmd, pktgen_credentials):

    pktgen_root = pktgen_credentials['root_dir']
    pktgen_logs_dir = pktgen_root + "logs/"

    if not os.path.exists(pktgen_logs_dir):
        os.makedirs(pktgen_logs_dir)
    cmd = "{}build/pktgen -l 0-35 -n 4 --log-level=1 -- {} > {}pktgen_console_logs.txt 2>&1".format(
        pktgen_root, pktgen_cmd, pktgen_logs_dir)
    run_cmd(cmd)
    while not os.path.exists("{}pktgen.lck".format(pktgen_root)):
        pass



#TODO: Optimize this function
def prepare_cpfs_action_arguments(actions):
    
    str_cmb = lambda x: ",".join(x)

    flat_list = []
    for i in range(len(actions)):
        sp_list = []
        for sublist in list(actions[i].items()):
            for item in sublist:

                if item == 'crash' or item == 'type':
                    sp_list.append('0')
                elif item == 'straggler' or item == 'frequency':
                    sp_list.append('1')
                else:
                    sp_list.append(str(item))

        flat_list.append(sp_list)
    
    return "_".join(list(map(str_cmb, flat_list)))

def prepare_cpfs_replicas_arguments(replicas):
    str_cmb = lambda x: ",".join(x)

    flat_list = []
    for i in range(len(replicas)):
        sp_list = []
        for sublist in list(replicas[i].items()):
            for item in sublist:
                if isinstance(item, list):
                    sp_list.append(",".join(item))
                else:        
                    sp_list.append(item)

        flat_list.append(sp_list)
    
    return "_".join(list(map(str_cmb, flat_list)))

def gen_configs(config):
    
    proc = 0
    if config["procedure"] == "attach":
        proc = 1
    elif config["procedure"] == "handover":
        proc = 2
    else:
        proc = 3
    
    cta_conf = {
        "cpfs": config["cpfs"],
        "remote_cpfs": config["remote_cpfs"],
        "replicas": config["replicas"],
        "remote_replicas": config["remote_replicas"],
        "tx_arg": config["tx_arg"],
        "delay": config["delay"],
        "procedure": proc,
        "scheme": 0 if config["scheme"] == "asn1" else 1,
        "cpfs_action": prepare_cpfs_action_arguments(config["cpfs_action"]),
    }


    pktgen_conf = {
        "duration":     config["duration"],
        "workers":      config["workers"],
        "scheme":       config["scheme"],
        "procedure":    config["procedure"],
        "rate":         config["curr_rate"] // config["workers"],
    }
    return pktgen_conf, cta_conf


def stop_pktgen():
    run_cmd("pkill --signal SIGINT pktgen")
    while os.path.exists('pktgen.lck'):
        pass


def collect_logs(config, exp_id, servers_credentials):

    pktgen_credentials = servers_credentials['pktgen']
    core_credentials = servers_credentials['core_network']

    pktgen_logs_dir = pktgen_credentials['root_dir'] + "logs/"
    cta_logs_dir = core_credentials['cta']['root_dir'] + "logs/"
    cpf_logs_dir = core_credentials['cpf']['root_dir'] + "logs/"

    # Move Pktgen logs to results dir
    config.update({"exp_id": exp_id})
    res_path = make_res_dir(config, pktgen_credentials)

    print("Collecting Pktgen Logs")
    for pktgen_log in pktgen_log_files:
        src = os.path.join(pktgen_logs_dir + pktgen_log)
        if exists(src):
            dst = os.path.join(res_path + pktgen_log)
            move(src, dst)

    # Fetch CTA logs to results dir
    print("Collecting CTA Logs")
    with SCPClient(cta_conn.get_transport()) as scp:
        for cta_log in cta_log_files:
            scp.get(cta_logs_dir + cta_log, local_path=res_path)
            
    print("Collecting CPF Logs")
    with SCPClient(cpf_conn.get_transport()) as scp:
        for cpf_log in cpf_log_files:
            scp.get(cpf_logs_dir + cpf_log, local_path=res_path)


def print_desc(exp_conf, exp_id):
    desc = pktgen_desc.substitute({
        "id": exp_id,
        "scheme": exp_conf["scheme"],
        "proc": exp_conf["procedure"],
        "rate": exp_conf["curr_rate"],
        "mode": "bursty" if exp_conf["bursty"] else
                "uniform",
        "delay": exp_conf["delay"],
        "tx_arg": exp_conf["tx_arg"],
        "remote_replicas": exp_conf["remote_replicas"],
        "local_replicas": (exp_conf["replicas"] - exp_conf["remote_replicas"])
    })
    print(desc)

def run_exp(exp_id, exp_conf, rate, proc_count, servers_credentials):
    global cta_conn
    global cpf_conn
    print("============= Starting New Experiment =============")
    exp_conf["curr_rate"] = rate

    print_desc(exp_conf, exp_id)

    pktgen_conf, core_conf = gen_configs(exp_conf)

    core_credentials = servers_credentials["core_network"]
    pktgen_credentials = servers_credentials["pktgen"]

    cpf_root = core_credentials['cpf']['root_dir']
    cta_root = core_credentials['cta']['root_dir']
    
    cpf_logs_dir = cpf_root + "logs/"
    cta_logs_dir = cta_root + "logs/"

    cpf_conn, cpf_chan = connect_entity(core_credentials['cpf']['ip'], 
                                        core_credentials['cpf']['username'],
                                        pktgen_credentials['public_key_path'])
    
    run_core(cpf_conn, cpf_chan, cpf_root, cpf_logs_dir, 'cpf', parse_cpf_args(core_conf),
             core_credentials['cpf'])
    print("Started CPF(s)")

    cta_conn, cta_chan = connect_entity(core_credentials['cta']['ip'], 
                                        core_credentials['cta']['username'],
                                        pktgen_credentials['public_key_path'])
    run_core(cta_conn, cta_chan, cta_root, cta_logs_dir, 'cta', parse_cta_args(core_conf),
             core_credentials['cta'])
    print("Started CTA")

    pktgen_conf["proc_count"] = proc_count
    run_pktgen(parse_pktgen_args(pktgen_conf), pktgen_credentials)
    print("Started pktgen")

    duration = exp_conf["duration"]
    sleep(duration)

    print("Stopping pktgen")
    stop_pktgen()
    print("Stopping CTA")
    stop_core('cta', cta_conn, cta_root, core_credentials['cta'])
    print("Stopping CPF(s)")
    stop_core('cpf', cpf_conn, cpf_root, core_credentials['cpf'])
    print("Collecting logs")

    sleep(15)
    if exp_conf["bursty"]:
        exp_conf["curr_rate"] = proc_count
    collect_logs(exp_conf, exp_id, servers_credentials)
    cta_conn.close()
    cpf_conn.close()
    print("Experiment done ... !!!")


def run_burst_experiment(confs, exp_id, rate, servers_credentials):
    for proc_count in confs["proc_count"]:
        run_exp(exp_id, confs, rate, proc_count, servers_credentials)


def run_uniform_experiment(confs, exp_id, rate, servers_credentials):
    run_exp(exp_id, confs, rate, 0, servers_credentials)


def run_from_configs(confs, servers_credentials):
    repeat = confs["repeat"]

    # For each experiment
    for exp in confs["experiments"]:
        # For every repeat
        for exp_id in range(1, repeat + 1):
            # For each rate value
            for rate in exp["rate"]:
                # For each iteration
                if exp["bursty"]:
                    run_burst_experiment(exp, exp_id, rate, servers_credentials)
                else:
                    run_uniform_experiment(exp, exp_id, rate, servers_credentials)


def sigint_handler(*args):
    global cta_conn
    global cpf_conn

    core_credentials = args[0]["core_network"]
    cta_root = core_credentials['cta']['root_dir']
    cpf_root = core_credentials['cpf']['root_dir']

    stop_pktgen()
    stop_core('cta', cta_conn, cta_root, core_credentials['cta'])
    stop_core('cpf', cpf_conn, cpf_root, core_credentials['cpf'])
    exit(0)


def clean_logs(servers_credentials):
    pktgen_root = servers_credentials['pktgen']['root_dir']
    pktgen_res_dir = pktgen_root + "results/"

    if os.path.exists(pktgen_res_dir):
        print("Removing {} because of --clean flag".format(pktgen_res_dir))
        rmtree(pktgen_res_dir)

def main():
    args = arg_parser()

    confs = load_config(pktgen_conf)
    servers_credentials = load_config(servers_credentials_conf)

    if args.clean:
        clean_logs(servers_credentials)

    signal(SIGINT, partial(sigint_handler, servers_credentials))

    run_from_configs(confs, servers_credentials)


if __name__ == "__main__":
    main()
