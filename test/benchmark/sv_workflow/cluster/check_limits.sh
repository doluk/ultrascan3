#!/usr/bin/env bash
# Print what limits how many svbench jobs a cluster can run:  cores per
# node, partitions and their time limits, array/job limits, priority and
# preemption, your QOS/association limits, and the current queue.
echo "== nodes (cores, memory, state)";  sinfo -N -o "%N %c %m %T %P"
echo; echo "== partitions";  sinfo -o "%P %a %l %D %c %m"
echo; echo "== scheduler config"
scontrol show config | grep -E \
  "MaxArraySize|MaxJobCount|SchedulerType|SchedulerParameters|PriorityType|PreemptType|PreemptMode|SelectTypeParameters|DefMemPer|MaxMemPer|AccountingStorageType"
echo; echo "== partition details";  scontrol show partition
echo; echo "== your association / QOS limits (needs accounting)"
sacctmgr -n -P show assoc user="$USER" \
  format=Cluster,Account,Partition,QOS,MaxJobs,MaxSubmit,MaxTRES,MaxWall 2>/dev/null \
  || echo "(no accounting)"
sacctmgr -n -P show qos format=Name,Priority,MaxJobsPU,MaxSubmitPU,MaxTRESPU,MaxWall 2>/dev/null
echo; echo "== can jobs submit jobs? (sbatch on compute nodes)"
srun -n1 -t 1 sh -c 'command -v sbatch || echo "sbatch NOT found on compute node"' 2>&1
echo; echo "== MPI";  command -v mpirun srun;  srun --mpi=list 2>&1 | head -5
echo; echo "== queue now"
squeue -o "%.10i %.10P %.10u %.2t %.10M %.10l %.5C %R" | head -40
