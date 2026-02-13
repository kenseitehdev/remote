
import pandas as pd
import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
from sklearn.model_selection import train_test_split
from sklearn.metrics import accuracy_score
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import time

torch.manual_seed(42)
np.random.seed(42)

col_names = ['size', 'shape', 'color', 'boundary', 'diagnosis']

df = pd.read_csv('dataset.csv', names=col_names)

print("Dataset loaded successfully!")
print(f"Shape: {df.shape}")
print(f"\nFirst few rows:\n{df.head()}\n")

print("Data types before conversion:")
print(df.dtypes)
print()

for col in col_names:
    df[col] = pd.to_numeric(df[col], errors='coerce')

df = df.dropna()

print(f"Shape after cleaning: {df.shape}")
print(f"\nData distribution:\n{df['diagnosis'].value_counts()}\n")

X = df[['size', 'shape', 'color', 'boundary']].values.astype(np.float32)
y = df['diagnosis'].values.astype(np.float32)

X_train, X_test, y_train, y_test = train_test_split(
    X, y, test_size=0.2, random_state=42
)

X_train_tensor = torch.FloatTensor(X_train)
y_train_tensor = torch.FloatTensor(y_train).reshape(-1, 1)
X_test_tensor = torch.FloatTensor(X_test)
y_test_tensor = torch.FloatTensor(y_test).reshape(-1, 1)

print(f"Training samples: {len(X_train)}")
print(f"Testing samples: {len(X_test)}\n")


class SkinCancerNN(nn.Module):
    def __init__(self):
        super(SkinCancerNN, self).__init__()
        self.layer1 = nn.Linear(4, 16)
        self.layer2 = nn.Linear(16, 8)
        self.layer3 = nn.Linear(8, 1)
        self.relu = nn.ReLU()
        self.sigmoid = nn.Sigmoid()
    
    def forward(self, x):
        x = self.relu(self.layer1(x))
        x = self.relu(self.layer2(x))
        x = self.sigmoid(self.layer3(x))
        return x


model = SkinCancerNN()
criterion = nn.BCELoss()
optimizer = optim.Adam(model.parameters(), lr=0.01)

print("Neural Network Architecture:")
print(model)
print()

epochs = 50
epoch_times = []

print("SPEED TEST - Training for 50 epochs\n")

for epoch in range(epochs):
    start_time = time.time()
    
    outputs = model(X_train_tensor)
    loss = criterion(outputs, y_train_tensor)
    
    optimizer.zero_grad()
    loss.backward()
    optimizer.step()
    
    end_time = time.time()
    epoch_time = end_time - start_time
    epoch_times.append(epoch_time)
    
    if (epoch + 1) % 10 == 0:
        print(f"Epoch [{epoch+1}/{epochs}], Loss: {loss.item():.4f}, Time: {epoch_time:.4f}s")

print(f"\nAverage time per epoch: {np.mean(epoch_times):.4f}s")
print(f"Total training time: {np.sum(epoch_times):.4f}s\n")

model.eval()
with torch.no_grad():
    y_pred = model(X_test_tensor)
    y_pred_class = (y_pred >= 0.5).float()
    
    accuracy = accuracy_score(y_test_tensor.numpy(), y_pred_class.numpy())
    
    print("MODEL ACCURACY")
    print(f"Accuracy Score: {accuracy:.4f} ({accuracy*100:.2f}%)\n")

print("PROBABILITY PREDICTIONS\n")

use_case_1 = torch.FloatTensor([[1, 1, 1, 1]])
use_case_2 = torch.FloatTensor([[0, 0, 0, 0]])

with torch.no_grad():
    prob_1 = model(use_case_1)
    prob_2 = model(use_case_2)
    
    print(f"Use Case 1: [1, 1, 1, 1] (all markers present)")
    print(f"Probability of skin cancer: {prob_1.item():.4f} ({prob_1.item()*100:.2f}%)")
    
    print(f"\nUse Case 2: [0, 0, 0, 0] (no markers present)")
    print(f"Probability of skin cancer: {prob_2.item():.4f} ({prob_2.item()*100:.2f}%)\n")

print("DIAGNOSIS PREDICTIONS\n")

prediction_cases = torch.FloatTensor([
    [1, 1, 0, 0],
    [1, 1, 1, 1],
    [0, 0, 0, 1]
])

with torch.no_grad():
    predictions = model(prediction_cases)
    pred_classes = (predictions >= 0.5).int()
    
    cases_desc = [
        "[1, 1, 0, 0] - Size and shape markers",
        "[1, 1, 1, 1] - All markers present",
        "[0, 0, 0, 1] - Only boundary marker"
    ]
    
    for i, (case, prob, pred) in enumerate(zip(cases_desc, predictions, pred_classes)):
        print(f"Case {i+1}: {case}")
        print(f"  Probability: {prob.item():.4f} ({prob.item()*100:.2f}%)")
        print(f"  Diagnosis: {'POSITIVE' if pred.item() == 1 else 'NEGATIVE'} for skin cancer\n")

plt.figure(figsize=(12, 6))

plt.subplot(1, 2, 1)
plt.plot(range(1, epochs+1), epoch_times, marker='o', linestyle='-', linewidth=2, markersize=4)
plt.xlabel('Epoch', fontsize=12)
plt.ylabel('Time (seconds)', fontsize=12)
plt.title('Training Time per Epoch', fontsize=14, fontweight='bold')
plt.grid(True, alpha=0.3)

plt.subplot(1, 2, 2)
plt.bar(range(1, epochs+1), epoch_times, color='steelblue', alpha=0.7)
plt.xlabel('Epoch', fontsize=12)
plt.ylabel('Time (seconds)', fontsize=12)
plt.title('Training Time Distribution', fontsize=14, fontweight='bold')
plt.grid(True, alpha=0.3, axis='y')

plt.tight_layout()
plt.savefig('neural_network_speed_test.png', dpi=300, bbox_inches='tight')

print("Speed test plot saved to 'neural_network_speed_test.png'\n")

print("SPEED TEST STATISTICS")
print(f"Minimum time: {min(epoch_times):.4f}s (Epoch {epoch_times.index(min(epoch_times))+1})")
print(f"Maximum time: {max(epoch_times):.4f}s (Epoch {epoch_times.index(max(epoch_times))+1})")
print(f"Mean time: {np.mean(epoch_times):.4f}s")
print(f"Median time: {np.median(epoch_times):.4f}s")
print(f"Std deviation: {np.std(epoch_times):.4f}s")

print("\nAnalysis complete Check 'neural_network_speed_test.png' for visualization.")

