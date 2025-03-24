import os
from glob import glob
import pandas as pd
import plotly.graph_objects as go
from plotly.subplots import make_subplots
from plotly.io import write_image
import numpy as np

library_name_map = {
    'reference_blas' : "Reference BLAS",
    'openblas'       : "OpenBLAS",
    'blis'           : "BLIS"
}

def df_ify(globby):
    
    df = []
    for x in glob(globby, recursive=True):

        rows = {}
        library = x.split("/")[2]
        compiler = x.split("/")[-2].split("_")[3]

        if len(x.split("/")[-2].split("_")) > 4:
            options = " ".join(x.split("/")[-2].split("_")[4:])
            options = options.replace("-fp-model-fast-", "-fast=")
        else:
            options = "vanilla"

        lines = open(x).readlines()
        idx = 0
        while not lines[idx].strip().startswith("** selecting function executions for replay"):
            idx += 1

        idx += 1
        while not lines[idx].strip().startswith("** replaying function executions with nan overwrites"):
            if lines[idx].strip().startswith("saved") and "/" not in lines[idx]:
                function_name = lines[idx].split()[2]
                n_selected_executions = int(lines[idx].split()[1])
                label = library_name_map[library] + " + " + compiler

                if compiler == "ifx":
                    label += " -O3"

                if options != "vanilla":
                    label += " " + options
                else:
                    label += " (default)"

                rows[function_name] = {
                    'label' : label,
                    'library' : library,
                    'compiler' : compiler,
                    'options' : options,
                    'function_name' : function_name[:-1],
                    'n_selected_executions' : n_selected_executions,
                    'n_overwrites' : 0,
                    'n_warnings' : 0,
                    'n_true_warnings' : 0,
                    'warning_rate'       : float("nan"),
                    'true_positive_rate' : float("nan"),
                    'n_sat' : 0,
                    'n_event_traces' : 0,
                }
            idx += 1

        idx += 1
        while not lines[idx].strip().startswith("** attempting to generate inputs that reify exception-handling failures"):
            if lines[idx]:
                function_name = lines[idx].strip()
                
                if function_name not in rows:
                    idx += 1
                    continue

                idx += 1
                if "Segmentation" in lines[idx] or "signal" in lines[idx]:
                    idx += 1
                    rows[function_name]["warning_rate"] = -1 * float("inf")
                    rows[function_name]["true_positive_rate"] = -1 * float("inf")
                else:
                    n_overwrites = int(lines[idx].split()[-1])
                    rows[function_name]["n_overwrites"] = n_overwrites

                    idx += 1
        
                    n_warnings = int(lines[idx].split()[-1])
                    rows[function_name]["n_warnings"] = n_warnings

                    if n_overwrites > 0:
                        warning_rate = n_warnings/n_overwrites
                        rows[function_name]["warning_rate"] = int(warning_rate * 100) / 100

                    if n_warnings > 0:
                        n_true_warnings = len(glob(os.path.dirname(x) + "/" + function_name + "/*.out"))
                        rows[function_name]["n_true_warnings"] = n_true_warnings
                        rows[function_name]["true_positive_rate"] = int(n_true_warnings/n_warnings * 100) / 100

            idx += 1

        idx += 1
        while idx < len(lines):
            if lines[idx]:
                function_name = lines[idx].strip()
                
                if function_name not in rows:
                    idx += 1
                    continue

                idx += 1
                while ":" in lines[idx]:
                    idx += 1

                try:
                    n_sat = int(lines[idx].split()[0])
                    rows[function_name]["n_sat"] = n_sat
                except ValueError:
                    rows[function_name]["warning_rate"] = -1 * float("inf")
                    rows[function_name]["true_positive_rate"] = -1 * float("inf")
                    pass

                idx += 1
                if n_sat > 0:
                    n_event_traces = lines[idx].split()[0]
                else:
                    n_event_traces = 0
                rows[function_name]["n_event_traces"] = n_event_traces

            idx += 1

        df += list(rows.values())

    df = pd.DataFrame(df)
    df = df[df['compiler'] != 'ifort']
    df['library'] = pd.Categorical(df['library'], ['reference_blas', 'blis', 'openblas'])
    df['options'] = pd.Categorical(df['options'], ['vanilla', '-O3', '-ffast-math', '-fast=1', '-fast=2', '-O3 -ffast-math', '-O3 -fast=1', '-O3 -fast=2'])
    label_order = df.sort_values(by=['library','compiler', 'options', 'function_name'])['label'].unique()
    df['label'] = pd.Categorical(df['label'], list(label_order))

    return df

df = df_ify("artifact/results/**/__EXCVATE**/nohup.out")

df1 = df.sort_values(by=['label','function_name']).pivot(index='function_name', columns='label')['warning_rate']
df1 = df1.loc[df1.sum(axis=1).sort_values().index]

fig=go.Figure(
    data=go.Heatmap(
        z=df1.to_numpy(),
        y=df1.index,
        x=df1.columns,
        zmin = 0,
        zmax = 1,
        colorscale = 'blackbody',
    )
)
fig.update_layout(
    width=1000,
    height=350,  
    font_size=18,
    font_family="serif",
    margin={
        'l' : 80,
        'r' : 130,
        't' : 50,
        'b' : 0,
    },
    xaxis_showgrid=False,
    yaxis_showgrid=False,
    plot_bgcolor="#000000"
)
ticktext = list(df.sort_values(by=['label','function_name']).pivot(index='function_name', columns='label')["options"].replace("vanilla", "default").iloc[0].to_numpy())
for i in range(len(ticktext)):
    if ticktext[i].startswith("-"):
        ticktext[i] = "+ " + ticktext[i]
fig.update_xaxes(
    ticks="outside",
    ticklen=5,
    tickangle=40,
    tickmode='array',
    tickvals=list(range(0,24)),
    ticktext=ticktext,
    ticklabelstandoff=-10,
)
fig.add_annotation(
    text="Warning Rate",
    textangle=90,
    font_size=24,
    showarrow=False,
    xref="paper",
    yref="paper",
    x=1.152,
    y=0.5
)
title_x_pos=0.04
title_y_pos=1.265
subtitle_x_pos=0.05
subtitle_y_pos=1.13
fig.add_annotation(
    text="Reference BLAS",
    font_size=24,
    showarrow=False,
    xref="paper",
    yref="paper",
    x=title_x_pos,
    y=title_y_pos
)
fig.add_annotation(
    text="gfortran",
    font_size=18,
    showarrow=False,
    xref="paper",
    yref="paper",
    x=subtitle_x_pos,
    y=subtitle_y_pos,
)
fig.add_annotation(
    text="ifx",
    font_size=18,
    showarrow=False,
    xref="paper",
    yref="paper",
    x=subtitle_x_pos+0.17,
    y=subtitle_y_pos,
)
fig.add_annotation(
    text="BLIS",
    font_size=24,
    showarrow=False,
    xref="paper",
    yref="paper",
    x=title_x_pos+0.435,
    y=title_y_pos,
)
fig.add_annotation(
    text="gcc",
    font_size=18,
    showarrow=False,
    xref="paper",
    yref="paper",
    x=subtitle_x_pos+0.345,
    y=subtitle_y_pos,
)
fig.add_annotation(
    text="icx",
    font_size=18,
    showarrow=False,
    xref="paper",
    yref="paper",
    x=subtitle_x_pos+0.515,
    y=subtitle_y_pos,
)
fig.add_annotation(
    text="OpenBLAS",
    font_size=24,
    showarrow=False,
    xref="paper",
    yref="paper",
    x=title_x_pos+0.85,
    y=title_y_pos,
)
fig.add_annotation(
    text="gcc",
    font_size=18,
    showarrow=False,
    xref="paper",
    yref="paper",
    x=subtitle_x_pos+0.345 +0.365,
    y=subtitle_y_pos,
)
fig.add_annotation(
    text="icx",
    font_size=18,
    showarrow=False,
    xref="paper",
    yref="paper",
    x=subtitle_x_pos+0.515+0.365,
    y=subtitle_y_pos,
)
fig.add_annotation(
    text="srotmg",
    font_size=18,
    xanchor="right",
    xref="paper",
    yref="paper",
    axref="pixel",
    ayref="pixel",
    x=-0.01,
    y=0.98,
    ax=-20,
    ay=0,
    arrowhead=3,
    arrowsize=1.5
)
fig.add_annotation(
    text="srotm",
    font_size=18,
    xanchor="right",
    xref="paper",
    yref="paper",
    axref="pixel",
    ayref="pixel",
    x=-0.01,
    y=0.75,
    ax=-20,
    ay=0,
    arrowhead=3,
    arrowsize=1.5
)
fig.add_annotation(
    text="sger",
    font_size=18,
    xanchor="right",
    xref="paper",
    yref="paper",
    axref="pixel",
    ayref="pixel",
    x=-0.01,
    y=0.67,
    ax=-20,
    ay=0,
    arrowhead=3,
    arrowsize=1.5
)
fig.add_annotation(
    text="sgemv",
    font_size=18,
    xanchor="right",
    xref="paper",
    yref="paper",
    axref="pixel",
    ayref="pixel",
    x=-0.01,
    y=0.865,
    ax=-20,
    ay=0,
    arrowhead=3,
    arrowsize=1.5    
)
fig.add_annotation(
    text="sgbmv",
    font_size=18,
    xanchor="right",
    xref="paper",
    yref="paper",
    axref="pixel",
    ayref="pixel",
    x=-0.01,
    y=0.56,
    ax=-20,
    ay=0,
    arrowhead=3,
    arrowsize=1.5
)
fig.update_yaxes(
    showticklabels=False,
    ticks="outside",
    ticklen=5,
    tickmode='array',
    tickvals=list(range(0,26)),
)


fig.add_vline(x=3.5, line_color="#ffffff", line_dash="dash", line_width=1)
fig.add_vline(x=6.5, line_color="#ffffff", line_width=5)
fig.add_vline(x=10.5, line_color="#ffffff", line_dash="dash", line_width=1)
fig.add_vline(x=14.5, line_color="#ffffff", line_width=5)
fig.add_vline(x=18.5, line_color="#ffffff", line_dash="dash", line_width=1)
fig.show()
fig.write_html("artifact/results/figure_6.html")

with open("artifact/results/table_1_data.txt", "w") as f:
    for func_name in df[df["n_true_warnings"] != 0]["function_name"].unique():
        df_temp = df[df["function_name"] == func_name]
        print("" + func_name, file=f)
        print(f"\t# of         Spoofs: {df_temp['n_overwrites'].sum()}", file=f)
        print(f"\t# of       Warnings: {df_temp['n_warnings'].sum()} ({100*df_temp['n_warnings'].sum()/df_temp['n_overwrites'].sum():.2f}%)", file=f)
        print(f"\t# of True Positives: {df_temp['n_true_warnings'].sum()} ({100*df_temp['n_true_warnings'].sum()/df_temp['n_warnings'].sum():.2f}%)", file=f)

    print("--------------------------------------------------", file=f)
    print(f"\tTotal # of         Spoofs: {df['n_overwrites'].sum()}", file=f)
    print(f"\tTotal # of       Warnings: {df['n_warnings'].sum()} ({100*df['n_warnings'].sum()/df['n_overwrites'].sum():.2f}%)", file=f)
    print(f"\tTotal # of True Positives: {df['n_true_warnings'].sum()} ({100*df['n_true_warnings'].sum()/df['n_warnings'].sum():.2f}%)", file=f)