import subprocess
import os
import sys
import re
import matplotlib.pyplot as plt
import numpy as np

OPTIONS = [
    ("64k 8-way L2",            "dl2:128:64:8:l -cache:dl2lat 30"),
    ("64k 16-way L2 (BDI)",     "dl2:128:64:8:b -cache:dl2lat 32"),
    ("128k 8-way L2",           "dl2:256:64:8:l -cache:dl2lat 30")
]

INST = "20000000" 

BENCHMARKS = ["gcc", "go", "ijpeg", "li", "perl"]

def run_benchmark(benchmark_name, sim_path, options):
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
        "-args", f"-fastfwd {INST} -max:inst {INST} -cache:dl1 dl1:32:64:2:l -cache:il2 il2:1024:32:1:l -mem:lat 300 2 -cache:dl1lat 3 -cache:dl2 {options}"
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
    l2_hits_pattern = r"dl2\.hits\s+([0-9]+)"
    l2_misses_pattern = r"dl2\.misses\s+([0-9]+)"
    l2_comp_pattern = r"dl2\.bdi_compress_ratio\s+([0-9.]+)"

    ipc = 0.0
    l2_hits = 0
    l2_misses = 0
    l2_comp = 0

    ipc_match = re.search(ipc_pattern, output_text)
    if ipc_match:
        ipc = float(ipc_match.group(1))

    hits_match = re.search(l2_hits_pattern, output_text)
    if hits_match:
        l2_hits = int(hits_match.group(1))

    misses_match = re.search(l2_misses_pattern, output_text)
    if misses_match:
        l2_misses = int(misses_match.group(1))

    comp_match = re.search(l2_comp_pattern, output_text)
    if comp_match:
        l2_comp = float(comp_match.group(1))

    return ipc, l2_hits, l2_misses, l2_comp

def plot_results(benchmarks, baseline, bdi, baseline_2x, comp_data):
    """Generates two graphs: One for IPC, one for L2 Cache Stats."""
    print("\nGenerating graphs...")
    fig, ax2 = plt.subplots(1, 1, figsize=(12, 5))

    # --- Plot 1: IPC ---
    # ax1.bar(benchmarks, ipc_data, color='skyblue')
    # ax1.set_title("Instructions Per Cycle (IPC)")
    # ax1.set_ylabel("IPC")
    # ax1.set_ylim(0, max(ipc_data) * 1.2 if ipc_data else 1) 

    # --- Plot the three runs for each benchmark ---
    x = np.arange(len(benchmarks)) 
    width = 0.15  

    ax2.bar_label(ax2.bar(x - width,  baseline,       width,  label=OPTIONS[0][0],     color='skyblue'),     fmt='%.3f', fontsize=6)
    ax2.bar_label(ax2.bar(x,          bdi,            width,  label=OPTIONS[1][0],     color='lightgreen'),  fmt='%.3f', fontsize=6)
    ax2.bar_label(ax2.bar(x + width,  baseline_2x,    width,  label=OPTIONS[2][0],     color='yellow'),      fmt='%.3f', fontsize=6)

    ax2.set_ylim(0.8)
    ax2.set_title("Unified L2 Cache Performance")
    ax2.set_ylabel("Speedup")
    ax2.set_xticks(x)
    labels = [f"{b} (comp-ratio: {c})" for b, c in zip(benchmarks, comp_data)]
    ax2.set_xticklabels(labels)
    ax2.legend()

    plt.tight_layout()
    plt.savefig("benchmark_results.png")
    print("Graph saved as 'benchmark_results.png'")
    plt.show()

def main():
    sim_path = os.path.expandvars("$PWD/ss3/sim-outorder")
     
    valid_benchmarks = []
    ipc_data = []
    l2_hits_data = []
    l2_misses_data = []
    comp_data = []

    for bench in BENCHMARKS:
        for run, opt in OPTIONS:
            output_text = run_benchmark(bench, sim_path, opt)
            
            if output_text:
                ipc, hits, misses, comp = parse_metrics(output_text)
                print(f"Results for {bench} ({run}) -> IPC: {ipc}, L2 Hits: {hits}, L2 Misses: {misses}\n")
                
                if not (bench in valid_benchmarks): valid_benchmarks.append(bench)
                ipc_data.append(ipc)
                l2_hits_data.append(hits)
                l2_misses_data.append(misses)
                if comp != 0: comp_data.append(comp)
            
    if valid_benchmarks:
        ipc_baseline = ipc_data[0::3]
        ipc_bdi = ipc_data[1::3]
        ipc_baseline_2x = ipc_data[2::3]

        spu_baseline = []
        spu_bdi = []
        spu_baseline_2x = []

        i = 0
        for ipc in ipc_baseline:
            spu_baseline.append(ipc_baseline[i] / ipc)
            spu_bdi.append(ipc_bdi[i] / ipc)
            spu_baseline_2x.append(ipc_baseline_2x[i] / ipc)
            i += 1

        # Plot the speedup ratios
        plot_results(valid_benchmarks, spu_baseline, spu_bdi, spu_baseline_2x, comp_data)
    else:
        print("No valid data to plot.")

if __name__ == "__main__":
    main()