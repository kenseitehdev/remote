import gymnasium as gym
import numpy as np
from tensorflow import keras

# Load model
model = keras.models.load_model("cartpole_dqn.keras")

# Create environment
env = gym.make("CartPole-v1", render_mode="human")

state, _ = env.reset()
done = False

while not done:
    state_input = np.expand_dims(state, axis=0)
    action_probs = model.predict(state_input, verbose=0)
    action = np.argmax(action_probs[0])

    state, reward, done, truncated, _ = env.step(action)
    done = done or truncated

env.close()