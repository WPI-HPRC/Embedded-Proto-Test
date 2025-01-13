import subprocess
import os

print("#####################################################################")
print("#                  Generating code for .proto file                  #")
print("#####################################################################")

# Check if the 'generated' directory exists, if not, create it
generated_dir = "generated"
if not os.path.exists(generated_dir):
    os.makedirs(generated_dir)
    print(f"Directory '{generated_dir}' created.")
else:
    print(f"Directory '{generated_dir}' already exists.")

# Define the protoc command
command = [
    "protoc",
    "--plugin=protoc-gen-eams=lib/EmbeddedProto/protoc-gen-eams",
    "-I",
    "proto",
    "--eams_out=generated",
    "proto/node.proto",
]

# Run the command
result = subprocess.run(command, capture_output=True, text=True)

# Output the result
if result.returncode == 0:
    print("Code generation successful.")
else:
    print("Error in code generation:")
    print(result.stderr)
