import subprocess
import os
import sys
import re
import matplotlib.pyplot as plt
import numpy as np

def run_benchmark(benchmark_name, sim_path):
    """Runs the Perl script for a given benchmark, saves the .out file, and returns the text."""
    print(f"--- Running Benchmark: {benchmark_name} ---")
    
    # Create a dynamic results directory based on the benchmark name
    res_dir = f"results/{benchmark_name}"
    os.makedirs(res_dir, exist_ok=True)  # Create the folder if it doesn't exist
    
    # Define where the .out file will be saved
    out_file_path = os.path.join(res_dir, f"{benchmark_name}.out")
    
    command = [
        "perl", "./Run.pl", 
        "-db", "./bench.db", 
        "-dir", res_dir, 
        "-benchmark", benchmark_name, 
        "-sim", sim_path, 
        "-args", "-fastfwd 1000000 -max:inst 1000000 -cache:dl2 dl2:1024:64:4:b"
    ]

    try:
        # Capture stdout and stderr combined
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            check=True
        )
        
        # Combine standard output and error stream
        output_text = result.stdout + "\n" + result.stderr

        with open(out_file_path, "w") as outfile:
            outfile.write(output_text)
        print(f"Log saved successfully to: {out_file_path}")

        return output_text

    except subprocess.CalledProcessError as e:
        # If it crashes, we still want to save the error log to the .out file!
        error_text = (e.stdout or "") + "\n" + (e.stderr or "")
        
        with open(out_file_path, "w") as outfile:
            outfile.write(f"--- CRASHED WITH RETURN CODE {e.returncode} ---\n")
            outfile.write(error_text)
            
        print(f"Error running {benchmark_name}. Error log saved to {out_file_path}", file=sys.stderr)
        return None

def parse_metrics(output_text):
    """Parses the SimpleScalar output text to find IPC, L2 Hits, and L2 Misses."""
    ipc_pattern = r"sim_IPC\s+([0-9.]+)"
    l2_hits_pattern = r"ul2\.hits\s+([0-9]+)"
    l2_misses_pattern = r"ul2\.misses\s+([0-9]+)"

    ipc = 0.0
    l2_hits = 0
    l2_misses = 0

    ipc_match = re.search(ipc_pattern, output_text)
    if ipc_match:
        ipc = float(ipc_match.group(1))

    hits_match = re.search(l2_hits_pattern, output_text)
    if hits_match:
        l2_hits = int(hits_match.group(1))

    misses_match = re.search(l2_misses_pattern, output_text)
    if misses_match:
        l2_misses = int(misses_match.group(1))

    return ipc, l2_hits, l2_misses

def plot_results(benchmarks, ipc_data, l2_hits_data, l2_misses_data):
    """Generates two graphs: One for IPC, one for L2 Cache Stats."""
    print("\nGenerating graphs...")
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))

    # --- Plot 1: IPC ---
    ax1.bar(benchmarks, ipc_data, color='skyblue')
    ax1.set_title("Instructions Per Cycle (IPC)")
    ax1.set_ylabel("IPC")
    ax1.set_ylim(0, max(ipc_data) * 1.2 if ipc_data else 1) 

    # --- Plot 2: L2 Cache Hits vs Misses ---
    x = np.arange(len(benchmarks))  
    width = 0.35  

    ax2.bar(x - width/2, l2_hits_data, width, label='L2 Hits', color='lightgreen')
    ax2.bar(x + width/2, l2_misses_data, width, label='L2 Misses', color='salmon')

    ax2.set_title("Unified L2 Cache Performance")
    ax2.set_ylabel("Count")
    ax2.set_xticks(x)
    ax2.set_xticklabels(benchmarks)
    ax2.legend()

    plt.tight_layout()
    plt.savefig("benchmark_results.png")
    print("Graph saved as 'benchmark_results.png'")
    plt.show()

def main():
    sim_path = os.path.expandvars("/u/senkmuth/Desktop/adv_comp_arch/project/simple_scalar/ss3/sim-outorder")
    benchmarks_to_run = ["gcc", "go", "ijpeg"] 
    
    valid_benchmarks = []
    ipc_data = []
    l2_hits_data = []
    l2_misses_data = []

    for bench in benchmarks_to_run:
        output_text = run_benchmark(bench, sim_path)
        
        if output_text:
            ipc, hits, misses = parse_metrics(output_text)
            print(f"Results for {bench} -> IPC: {ipc}, L2 Hits: {hits}, L2 Misses: {misses}\n")
            
            valid_benchmarks.append(bench)
            ipc_data.append(ipc)
            l2_hits_data.append(hits)
            l2_misses_data.append(misses)
            
    if valid_benchmarks:
        plot_results(valid_benchmarks, ipc_data, l2_hits_data, l2_misses_data)
    else:
        print("No valid data to plot.")

if __name__ == "__main__":
    main()