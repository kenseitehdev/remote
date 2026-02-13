import pandas as pd
from pgmpy.models import BayesianNetwork
from pgmpy.estimators import MaximumLikelihoodEstimator, BayesianEstimator
from pgmpy.inference import VariableElimination
import matplotlib
matplotlib.use('Agg')  # Non-interactive backend for SSH
import matplotlib.pyplot as plt
import networkx as nx

col_names = ['size', 'shape', 'color', 'boundary', 'diagnosis']

df = pd.read_csv('dataset.csv', names=col_names)

print(f"Shape: {df.shape}")
print(f"\nFirst few rows:\n{df.head()}\n")
from pgmpy.estimators import HillClimbSearch, BicScore

hc = HillClimbSearch(df)
best_model = hc.estimate(scoring_method=BicScore(df))

print("Self-learned model structure:")
print(f"Edges: {best_model.edges()}\n")

model_auto = BayesianNetwork(best_model.edges())
model_auto.fit(df, estimator=MaximumLikelihoodEstimator)

plt.figure(figsize=(10, 8))
pos = nx.spring_layout(nx.DiGraph(model_auto.edges()))
nx.draw(nx.DiGraph(model_auto.edges()), pos, with_labels=True, 
        node_color='lightblue', node_size=3000, font_size=10, 
        font_weight='bold', arrows=True, arrowsize=20)
plt.title("Self-Learned DAG (Incorrect)")
plt.savefig('dag_selflearned.png', dpi=300, bbox_inches='tight')
print("Self-learned DAG saved to 'dag_selflearned.png'\n")
edges = [
    ('size', 'diagnosis'),
    ('shape', 'diagnosis'),
    ('color', 'diagnosis'),
    ('boundary', 'diagnosis')
]
print(f"Corrected edges: {edges}\n")

model = BayesianNetwork(edges)
model.fit(df, estimator=BayesianEstimator, prior_type='BDeu')

print("Model fitted using Bayesian estimation\n")

print("CONDITIONAL PROBABILITY DISTRIBUTIONS")

for cpd in model.get_cpds():
    print(f"\nCPD of {cpd.variable}:")
    print(cpd)

inference = VariableElimination(model)

print("DIAGNOSIS PROBABILITIES FOR 10 SAMPLES")

for i in range(min(10, len(df))):
    evidence = df.iloc[i][['size', 'shape', 'color', 'boundary']].to_dict()
    result = inference.query(variables=['diagnosis'], evidence=evidence)
    
    print(f"\nSample {i+1}:")
    print(f"Evidence: {evidence}")
    print(f"Actual diagnosis: {df.iloc[i]['diagnosis']}")
    print(f"Predicted probabilities:\n{result}")

plt.figure(figsize=(12, 8))
pos = nx.spring_layout(nx.DiGraph(model.edges()), k=2, iterations=50)
nx.draw_networkx_nodes(nx.DiGraph(model.edges()), pos, 
                       node_color='lightgreen', node_size=4000)
nx.draw_networkx_edges(nx.DiGraph(model.edges()), pos, 
                       arrows=True, arrowsize=25, arrowstyle='->', 
                       edge_color='gray', width=2)
nx.draw_networkx_labels(nx.DiGraph(model.edges()), pos, 
                       font_size=12, font_weight='bold')
plt.title("Final Corrected DAG - All Features Point to Diagnosis", 
          fontsize=14, fontweight='bold')
plt.axis('off')
plt.tight_layout()
plt.savefig('dag_final.png', dpi=300, bbox_inches='tight')
print("Final DAG saved to 'dag_final.png'")

print("\nAnalysis complete: Check the following files:")
print("  - dag_selflearned.png (initial self-learned structure)")
print("  - dag_final.png (corrected structure)")

