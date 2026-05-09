# **Autonomous Cyber-Physical Simulation and Testing via Model Context Protocol: Integrating FreeCAD, ROS2, and Gazebo**

The integration of artificial intelligence into engineering design and validation workflows represents a fundamental paradigm shift in the development of robotics and cyber-physical systems. Historically, the pipeline connecting mechanical design, software control, and physical simulation has been heavily fragmented, plagued by proprietary formats, disparate software ecosystems, and the necessity for manual, human-in-the-loop interventions. Engineers have traditionally relied on discrete tools, requiring painstaking manual labor to translate a complex 3D assembly into a robotic description format, instantiate it within a functional physics simulator, and subsequently execute middleware commands to evaluate its dynamic performance.

The advent of the Model Context Protocol (MCP) resolves this long-standing bottleneck by providing a standardized, bidirectional communication layer that allows Large Language Models (LLMs) to act as central orchestrators across the entire robotic engineering pipeline.1 By establishing a unified protocol for tool discovery and execution, the MCP architecture facilitates a seamless, automated workflow. This analysis details the architecture, implementation, and operational dynamics of a fully automated, AI-driven testing environment. By connecting FreeCAD via the freecad-mcp server and the RobotCAD workbench, Gazebo via the gazebo-mcp server, and the Robot Operating System 2 via various ros2-mcp implementations, an AI agent can iteratively design a 3D model, export it to a Unified Robot Description Format (URDF) or Simulation Description Format (SDF), instantiate it within a simulated environment, execute complex navigational tasks, and extract performance metrics to inform subsequent design iterations.

## **The Model Context Protocol as the Universal Integration Layer**

Introduced by Anthropic in late 2024, the Model Context Protocol was engineered to replace the fragmented, brittle, and custom-built API integrations that previously connected AI models to external systems.1 In the context of advanced robotic simulation and computer-aided design (CAD), MCP shifts the underlying LLM from a static code-generation utility into a dynamic, stateful agent capable of perceiving, reasoning about, and manipulating a highly complex engineering environment.1

The architecture relies on a rigorously defined client-server topography that operates over standardized transport mechanisms.

| Architectural Component | Functional Role in the Robotic Ecosystem | Implementation Characteristics |
| :---- | :---- | :---- |
| **MCP Host** | The execution environment containing the foundational LLM. This component orchestrates the overarching intent, interpreting natural language and planning multi-step logic. | Environments such as Claude Desktop, Cursor IDE, or custom LangGraph frameworks.1 |
| **MCP Client** | The intermediary routing component within the host that establishes, secures, and manages concurrent connections to external capability servers. | Translates LLM intent into JSON-RPC 2.0 format, managing tool discovery and capability negotiation.1 |
| **MCP Server** | Lightweight, highly specialized programs that expose specific operational capabilities, context, and data to the LLM via strictly defined schemas. | Implementations include freecad-mcp, gazebo-mcp, and ros2-mcp, acting as translators to underlying APIs.5 |
| **Transport Layer** | The communication medium facilitating data exchange. It ensures low-latency, bidirectional, and stateful interaction between the client and servers. | Utilizes JSON-RPC 2.0 messages over standard input/output (STDIO) for local execution, or Server-Sent Events (SSE) for remote architectures.1 |

In an advanced multi-server orchestration workflow, the MCP host instantiates parallel connections to multiple specialized MCP servers simultaneously. This orchestrator pattern allows the AI to dynamically discover tools across vastly different domains without requiring any custom middleware to bridge them.7 For example, the agent can invoke a tool from the FreeCAD MCP server to modify a parametric dimension of a robot chassis, immediately follow up with a tool from the Gazebo MCP server to reset the simulation physics, and finally trigger a ROS2 MCP server tool to publish a velocity command to evaluate the new chassis geometry.7

This seamless handoff between specialized capability servers is critical. It relies on the protocol's ability to maintain persistent session state, handle complex capability discovery during the initialization phase, and execute actions with context preservation.8 Consequently, the AI agent is not merely generating code; it is actively probing the environment, reading real-time responses, and iterating on its approach based on dynamic feedback.

## **Generative Parametric Modeling via FreeCAD MCP Integration**

The initial stage of the automated simulation pipeline involves the generation, modification, and validation of the 3D robotic model. FreeCAD, an open-source parametric 3D modeler, provides the foundational geometry, topological data, and mechanical constraints required for physical asset creation. Interaction with FreeCAD is facilitated by the freecad-mcp server, an integration that exposes the software's extensive Python API directly to the LLM.11

### **Architecture and Connection Modalities of the FreeCAD Server**

The freecad-mcp implementation provides a robust server-client architecture that allows the AI to execute arbitrary code and manipulate the document tree. The server can be configured to operate via different connection modalities to suit the specific deployment environment, typically utilizing XML-RPC on port 9875 or a JSON-RPC socket on port 9876\.11 The server acts as a bridge script, instantiated via the MCP configuration file (e.g., claude\_desktop\_config.json), which points directly to the Python executable managing the FreeCAD logic.12

For remote development or cloud-based testing architectures, the server supports binding to 0.0.0.0, allowing remote connections over specific, whitelisted IP ranges.14 This enables a centralized AI agent running in a high-compute cloud environment to manipulate a FreeCAD instance running on a dedicated local workstation.

### **Core Toolset and Topological Manipulation**

The FreeCAD MCP server exposes a highly detailed suite of tools designed to bridge the gap between natural language prompts and precise geometric modeling.11 Through these tools, the LLM can navigate the document hierarchy, instantiate primitives, and perform complex Boolean operations.

| FreeCAD MCP Tool | Description and Engineering Utility | Pipeline Significance |
| :---- | :---- | :---- |
| get\_scene\_info | Retrieves comprehensive object data, including coordinate positions, rotation matrices, bounding box dimensions, and shape properties. | Provides the LLM with the spatial awareness required to make precise, context-aware geometric adjustments.12 |
| create\_object & edit\_object | Generates primitives (boxes, cylinders, spheres) and modifies existing parametric dimensions based on numerical inputs. | Enables iterative design changes, such as widening a wheelbase or thickening a load-bearing linkage.16 |
| execute\_code | Runs arbitrary Python scripts within the active FreeCAD context, allowing for complex topological manipulation. | Empowers the LLM to bypass simple tools and write highly optimized, multi-step macros for complex sweeps and lofts.17 |
| run\_fem\_analysis | Executes the CalculiX solver on an existing Fem::FemAnalysis object to return stress, displacement, and node count metrics. | Ensures structural validation of the model before it is exported for kinematic simulation.14 |
| insert\_part\_from\_library | Selects and inserts ready-made 3D models and mechanical components from an established parts library. | Accelerates the assembly of complex robots by utilizing pre-validated standardized components.14 |

The inclusion of the run\_fem\_analysis tool is particularly critical for establishing a rigorous engineering pipeline.14 Before the robotic model is ever exported to Gazebo for dynamic kinematic testing, the LLM can command FreeCAD to perform a Finite Element Method (FEM) analysis. By leveraging the open-source CalculiX solver, the AI can assess the structural integrity of the design under expected load conditions. If the maximum von Mises stress extracted from the analysis exceeds the chosen material's yield strength, the LLM can autonomously invoke the edit\_object tool to increase the wall thickness or add fillets to stress concentrators, continuously iterating on the design prior to dynamic simulation.

Furthermore, the FreeCAD architecture relies heavily on its Python console. The AI agent can utilize the execute\_code tool to automate the creation of parts from external data structures, such as CSV files parsed via the Pandas library, dynamically injecting these values as variables to construct highly parameterized, scalable structures.20 This headless capability is vital for autonomous operations, allowing the entire CAD generation process to occur without invoking the graphical user interface.22

## **The Transcoding Bridge: RobotCAD and URDF Generation**

A standard 3D mesh, consisting purely of vertices and faces, is insufficient for physical robotic simulation; the model must be meticulously augmented with kinematic relationships, inertial property tensors, and collision geometries. This highly complex translation is handled by the FreeCAD RobotCAD workbench, also known as CROSS, which converts standard CAD assemblies into ROS2-compatible description packages.24

### **Structural Conversion and Kinematic Definition**

RobotCAD operates through an extensive Python codebase that integrates directly into the FreeCAD environment, facilitating the conversion of a standard Assembly Workbench structure into a hierarchical link-and-joint structure required by the URDF standard.24

When instructed by the AI agent via the MCP interface, the workflow proceeds through several deeply automated phases. First, the agent leverages the built-in structural converter to define the rigid bodies (links) and the degrees of freedom (joints) connecting them. The workbench allows for the automatic creation of joints by simply selecting the faces of adjacent links, intuitively calculating the necessary coordinate transformations and rotation axes based on the Local Coordinate Systems (LCS).24

For rapid prototyping, the AI agent can utilize RobotCAD's integrated generative tools to create a base robot structure from primitives using pure text descriptions, establishing a rudimentary kinematic chain that can be subsequently refined.24

### **Inertial Calculation and Collision Simplification**

A precise physics simulation in Gazebo demands accurate inertial parameters. RobotCAD automatically calculates the mass, the center of mass in both global and local coordinates, and the complex ![][image1] inertia tensor matrix for every rigid body link within the assembly.24 These calculations are derived dynamically based on the volumetric data of the meshes and the specific material properties assigned from the workbench's integrated material library.24

Simultaneously, the workbench addresses the computational overhead of simulation. High-polygon visual meshes severely degrade the performance of physics engines tracking thousands of potential contact points. To mitigate this, RobotCAD features automated collision generation tools. Based on the "Real" element of a robotic link, the system can autonomously generate simplified collision objects, such as axis-aligned enclosing boxes, bounding spheres, or simplified cylinders, ensuring that the Gazebo solver maintains a high real-time execution factor without sacrificing kinematic accuracy.24

### **Code Generation and Containerization**

The final phase of the FreeCAD workflow is the actual exportation of the robotic package. RobotCAD features a "Basic Code Generator" capable of producing a comprehensive, ready-to-deploy ROS2 workspace package.24 This package includes the finalized URDF or SDF file detailing the kinematics, mass, inertia, and integrated Gazebo sensor plugins. It also automatically generates launch files for both Gazebo and RViz, alongside ros2\_controllers configurations required for actuation.24

For more complex deployment environments, the AI agent can trigger the "External Code Generating Service." This advanced module extends the basic generator by synthesizing an entirely containerized architecture. It generates highly specific Dockerfiles and startup scripts that encapsulate all dependencies, including ROS2, Gazebo, and hardware-specific configurations such as Nvidia container support for GPU acceleration.24 Consequently, the MCP agent can seamlessly dictate the compilation of the model and instantly deploy it into a completely isolated, reproducible, and headless containerized testing environment.

## **Physics Emulation and Dynamic Environments via Gazebo MCP**

Once the URDF package is generated and the containerized workspace is initialized, the robotic model must be instantiated within a realistic physical environment. Gazebo—specifically modern iterations such as Gazebo Harmonic or Gazebo Ionic—serves as the high-fidelity 3D physics simulator for this pipeline, offering substantial performance and usability improvements over the deprecated Gazebo Classic.26 Interaction with the simulator is entirely managed by the gazebo-mcp server, which provides the AI assistant with a standardized interface to manipulate environments, spawn entities, and gather vast arrays of sensor data.29

### **Physics Engine Selection and Optimization**

The operational validity of the imported model depends heavily on the underlying physics engine and its configuration. Gazebo supports multiple distinct physics engines, including the Open Dynamics Engine (ODE), Bullet, Simbody, and DART.28 The AI agent can utilize the MCP interface to adjust the simulation parameters, carefully balancing computational speed against the necessity for physical realism.

| Physics Engine | Solver Characteristics | Target Use Case in Simulation Pipeline |
| :---- | :---- | :---- |
| **DART** | Often the default in modern Gazebo. Excellent for articulated dynamics and rigid body kinematics. | Complex humanoid or multi-joint robotic arm simulations requiring high stability in joint constraints.28 |
| **ODE** | Fixed-step solver, highly mature and widely utilized. | General-purpose mobile robotics, wheeled navigation, and environments demanding consistent execution times.28 |
| **Bullet** | Highly optimized for game development and continuous collision detection. | Environments with massive quantities of interacting objects or complex collision meshes.28 |
| **Simbody** | Variable time-step solver designed for biomechanics and molecular dynamics. | High-precision modeling of internal forces and complex constraint resolutions.28 |

Key parameters controlled via the MCP interface include max\_step\_size and real\_time\_update\_rate. The product of these two values defines the upper bound of the Real-Time Factor (RTF).28 If the LLM monitoring the simulation detects that an intricate mechanical linkage imported from FreeCAD is suffering from collision clipping or numerical instability, it can autonomously invoke the gazebo\_set\_world\_property tool to decrease the max\_step\_size (e.g., modifying it from ![][image2] seconds to ![][image3] seconds). Furthermore, the agent can leverage the collision mesh optimization algorithms introduced in Gazebo Ionic, seamlessly striking a balance between high-fidelity physics and necessary simulation speed.27

### **Procedural World Generation and Fleet Coordination**

The gazebo-mcp server exposes a vast array of environmental controls that allow the AI to procedurally generate testing conditions, systematically validating the FreeCAD model against myriad edge cases.26

Using tools such as gazebo\_spawn\_model and gazebo\_spawn\_sdf, the agent injects the newly compiled URDF directly into the active simulation.29 The LLM possesses full authority over the initial spatial configurations, dictating the starting pose, orientation, and velocity of the model.

Beyond the robot itself, the environment is highly mutable. The server supports the dynamic placement of both static boundaries and dynamic primitive shapes or custom meshes, allowing the LLM to construct complex obstacle courses on the fly.26 To test navigation algorithms against unstructured terrain, the AI can orchestrate profound terrain modifications, invoking heightmap-based generation or altering surface friction materials to simulate the difference between concrete, sand, and gravel.26 Additionally, lighting controls allow the AI to adjust ambient, directional, and spot lights, or even simulate day/night cycles to validate the robustness of the robot's visual perception systems under varying lux levels.26

For testing multi-agent logistics or swarm logic scenarios, gazebo-mcp features advanced fleet spawning algorithms. The AI can command the simultaneous placement of complex formations, such as auto-sized ![][image4] grids, circular arrangements facing a central point, or collision-free randomized distributions of multiple robot instances.29 Token-efficient fleet monitoring tools provide the LLM with compressed, summarized statistical telemetry regarding active, moving, and idle units across the entire swarm, preventing context-window overflow during massive simulations.29

### **Sensor Emulation and Data Streaming**

To properly evaluate the performance of the mechanical model and its software stack, the AI must be able to accurately perceive the simulated world. The gazebo-mcp server provides exceptionally deep integration with Gazebo's comprehensive sensor suite.29

The toolset includes gazebo\_list\_sensors, which permits the LLM to discover available data streams dynamically, filtering by specific models or sensor types.29 Once a sensor is identified, the gazebo\_get\_sensor\_data tool retrieves immediate, singular snapshots of the environment.29

However, robotic control often requires continuous data flows rather than discrete snapshots. To accommodate this, the gazebo\_subscribe\_sensor\_stream tool allows the MCP server to subscribe directly to a sensor topic, caching the high-frequency data—such as LiDAR point clouds, IMU acceleration arrays, force-torque measurements, and GPS coordinates—and presenting the LLM with structured, rate-limited JSON representations of the physical state.29 This intelligent caching mechanism prevents the LLM from being overwhelmed by the sheer volume of data generated by a ![][image5] LiDAR scanner, aggregating the information into manageable contextual updates.

## **Middleware Synchronization via ROS2 MCP Architectures**

While Gazebo excels at calculating physics and rendering sensor data, the actual behavioral control logic, path planning, and inter-process communication of the robot are managed by the Robot Operating System 2 (ROS2). To achieve full closed-loop autonomy, the AI agent utilizes a ROS2 MCP server to bridge the Data Distribution Service (DDS) network directly into the LLM's context window.32

The open-source ecosystem provides several distinct architectural patterns for the ROS2 MCP bridge. Each implementation possesses a unique design philosophy tailored to different operational complexities, ranging from simple velocity commands to profound system introspection.

### **Comparative Analysis of ROS2 MCP Implementations**

The selection of the specific ROS2 MCP server dictates the depth of control the AI exerts over the simulation.

| Server Implementation | Architectural Philosophy | Core Capabilities | Pipeline Utility |
| :---- | :---- | :---- | :---- |
| **kakimochi/ros2-mcp-server** | Minimalist and focused execution. | Runs as a native rclpy node via the fastmcp framework. Exposes time-based duration tools publishing geometry\_msgs/Twist directly to /cmd\_vel.34 | Ideal for rapid behavioral prototyping and simple motion control (e.g., commanding the robot to "move forward for 5 seconds at 0.5 m/s").34 |
| **robotmcp/ros-mcp-server** | General-purpose, full-stack control. | Operates without altering robot source code, leveraging a rosbridge WebSocket. Supports bi-directional communication, service calls, and action goals.35 | Best for complex agentic workflows where the LLM must interact with custom message types, set parameters, and execute multi-stage navigation stacks.35 |
| **wise-vision/ros2\_mcp** | Dynamic bridging and data retention. | Features dynamic service/topic exposure and automatic type conversion. Includes a "Data Black Box" for retrieving past messages and automatic Quality of Service (QoS) selection.37 | Powerful for debugging asynchronous communication errors and performing historical telemetry analysis over long-duration simulations.37 |
| **LCAS/ros2\_mcp** | Deep system introspection and research. | Provides tools for interface analysis, real-time health monitoring, and direct image retrieval specifically mapped for Vision Language Models (VLMs).32 | Crucial for visual perception tasks, complex semantic reasoning, and validating the software state of the entire ROS2 node graph.32 |

### **Semantic Discovery and Autonomous Action Execution**

A critical and defining advantage of the MCP architecture is dynamic semantic discovery. In traditional workflows, integrating a new hardware sensor or custom algorithm required manually writing extensive serialization and deserialization wrappers.39 Under the MCP paradigm, the AI agent can invoke tools like ros2\_list\_topics or ros2\_list\_actions to autonomously explore the ROS2 node graph that was spawned by the RobotCAD launch scripts.36

If the LLM decides to append a custom sensor payload to the FreeCAD model, the robotmcp or LCAS server automatically resolves the custom message types, inspecting the fields and informing the LLM of the precise data structure required to interact with it.36 This abstraction ensures that the agent can interact with any hardware configuration without prior manual configuration.

Once the interfaces are successfully discovered, the AI orchestrates the physical movement. While simple implementations might rely on rudimentary /cmd\_vel publishing, an advanced LLM agent utilizes tools like ros2\_send\_action\_goal to interface directly with the Nav2 navigation stack.40 The agent provides a semantic target coordinate, allowing the simulated robot to autonomously calculate an optimal path, generate a trajectory, and execute movements to avoid the procedural obstacles previously spawned via the Gazebo MCP server.

### **Vision Language Model Integration**

For tasks requiring complex environmental understanding, the LCAS/ros2\_mcp implementation provides specialized tools for Vision Language Models (VLMs).32 By establishing direct retrieval pathways from ROS2 sensor\_msgs/Image topics, the MCP server bypasses standard text-based tokenization, feeding raw pixel data directly into the visual cortex of advanced models like Claude 3.5 Sonnet or GPT-4o.32 This allows the AI agent to visually confirm that the physical chassis designed in FreeCAD is not occluding the field of view of the simulated camera mounted on the robot, validating complex spatial and perceptual relationships.

## **Automated Validation and Quantitative Performance Metrics**

The culmination of seamlessly integrating freecad-mcp, gazebo-mcp, and ros2-mcp is the establishment of a fully automated, headless continuous integration and continuous deployment (CI/CD) pipeline for physical engineering assets.42 Robotics engineering is aggressively shifting away from localized lab testing toward continuous delivery models, where every single algorithmic code commit or CAD dimension modification triggers a comprehensive validation sequence.42

### **The Autonomous CI/CD Workflow**

The execution loop orchestrated by the LLM unfolds autonomously through several distinct phases:

1. **Initiation & Synthesis:** The AI agent receives a high-level prompt (e.g., "Design and validate an autonomous inspection rover capable of traversing a 15-degree incline").  
2. **Parametric Design & Export:** The agent iterates on the chassis geometry via freecad-mcp, executes a structural FEM analysis via CalculiX, and invokes a headless Python script through RobotCAD to generate the ROS2 workspace containing the compiled URDF.14  
3. **Environment Instantiation:** The agent commands gazebo-mcp to launch a headless Docker container, loading an unstructured industrial terrain world, and injects the new URDF model into the scene.29  
4. **Operational Execution:** Through ros2-mcp, the agent activates the SLAM and Nav2 controllers, transmitting a series of action goals to force the simulated robot to navigate the environment.40  
5. **Telemetry Extraction:** Concurrently, the agent utilizes both gazebo-mcp (extracting ground-truth physics state) and ros2-mcp (extracting robot-perceived telemetry) to subscribe to critical sensor streams, continuously logging IMU acceleration, wheel odometry, and contact sensor impacts into structured datasets.29

### **Advanced Quantitative Evaluation Metrics**

To rigorously evaluate the operational performance of the generated mechanical model and its software stack, the AI agent relies on quantitative metrics rather than subjective visual observation. By leveraging the vast data arrays collected during the simulation runs, the system calculates several vital Key Performance Indicators (KPIs).43

| Performance Metric | Definition and Calculation | Engineering Assessment Value |
| :---- | :---- | :---- |
| **Real-Time Factor (RTF)** | The mathematical ratio of simulated time progression to real-world computational time. | Directly indicates the computational footprint and efficiency of the collision meshes generated by FreeCAD. A low RTF suggests overly complex polygons choking the Gazebo solver.45 |
| **Mean Time to Traverse (MTT)** | The average temporal duration required for the simulated robot to successfully navigate between predefined spatial waypoints. | Evaluates the kinetic efficiency of the mechanical design and the tuning of the ROS2 path-generation algorithms.43 |
| **Total Collisions (TC)** | An aggregate integer count derived from Gazebo contact sensors or ROS2 bumper topics registering impact events. | Highlights severe flaws in the physical dimensions (e.g., a chassis designed too wide for standard corridors) or critical blind spots in the sensor array placement.43 |
| **Velocity Over Rough Terrain (VORT)** | The sustained forward linear velocity maintained when traversing procedurally generated uneven surfaces. | Validates the suspension mechanics, center of mass calculations, and material friction coefficients defined initially within the FreeCAD environment.43 |
| **Success Rate (SC)** | The overall percentage of successful waypoint arrivals without catastrophic failures, deadlocks, or mechanical rollovers. | Provides a high-level binomial metric critical for CI/CD regression testing across vast arrays of randomized environments.43 |

If the system registers an exceedingly high Total Collisions (TC) metric due to the robot struggling to clear tight obstacles, the LLM is capable of interpreting this structured feedback, formulating an engineering hypothesis, and looping back to the initial step. It reconnects to freecad-mcp, commands a reduction in the specific parameter governing chassis width, regenerates the URDF, and autonomously re-runs the simulation. This creates a self-healing, evolutionary cyber-physical design loop.46

### **The "1000-Room Challenge" and Environmental Heterogeneity**

To ensure absolute statistical significance and robust validation, the automated framework executes what is often termed the "1000-Room Challenge".42 Rather than testing the robotic model in a single, static warehouse, the AI orchestrator leverages gazebo-mcp to procedurally generate hundreds of entirely different layouts, randomizing obstacle density, corridor width, lighting, and terrain friction.29

The ROS2 algorithms are systematically tested against the fixed CAD model across all these variations. This methodology enforces environmental heterogeneity, mitigating the risk of the robot overfitting to a specific spatial configuration and forcing the emergence of highly obscure edge cases.42 If the navigation stack achieves a 99% Success Rate (SC) across all 1,000 procedurally generated environments, the engineering team is provided with a statistically significant validation of the product's reliability before a single physical component is manufactured.42

## **Advanced Diagnostics: Energy Consumption and Adaptive Communication**

Beyond standard kinematics, advanced ROS2 MCP integrations facilitate testing of non-functional requirements, such as energy efficiency and network stability. In mobile robotics operating on strict power budgets, the wireless communication of sensor data and commands via DDS can severely drain battery reserves.48

During simulation, the MCP agent can implement and monitor an adaptive communication framework based on ROS2 messages. By tracking the Received Signal Strength Indicator (RSSI) as a feedback metric to estimate link quality, the simulated robot can dynamically compute the optimal combination of transmission power and data rate.48 If the MCP agent detects that link quality is degrading due to simulated environmental interference (e.g., traversing behind a large, dense obstacle spawned in Gazebo), it can analyze the system's message prioritization logic. The agent validates whether the robot correctly reduces the transmission frequency of lower-priority telemetry while maintaining essential control streams, thereby minimizing energy consumption and preserving communication reliability.48 This level of diagnostic testing ensures that the software stack is resilient to the realities of imperfect physical hardware.

## **Resolving the Sim-to-Real Gap and Architectural Limitations**

While the capability of this tri-server architecture is vast, integrating highly deterministic systems—FreeCAD's topological math, Gazebo's rigid-body physics, and ROS2's message passing—with the inherently non-deterministic logic of Large Language Models presents unique architectural tensions.46

### **Token Consumption and Headless Scripting**

As the complexity of the robot increases, the number of available tools exposed across the freecad-mcp, gazebo-mcp, and ros2-mcp servers scales exponentially into the hundreds. Loading all of these intricate tool schemas and JSON definitions into the LLM's context window upfront becomes computationally exorbitant, significantly increasing inference latency and API costs.49

To mitigate this, advanced MCP architectures are rapidly shifting from granular, direct tool-calling sequences toward programmatic code execution.49 Instead of the LLM invoking a dozen separate edit\_object and create\_cylinder tools sequentially to build a wheel assembly, the agent synthesizes a single, highly optimized Python script that performs the entire CAD operation locally. It then passes this script to the execute\_code tool within freecad-mcp.18 This methodology drastically reduces the token overhead of passing intermediate, high-density geometric structures or massive LiDAR point-clouds back and forth through the LLM context window, keeping the heavy computational lifting strictly localized within the specific server environments.49

### **Bridging the Deterministic Boundary**

The flexibility of natural language reasoning is inherently prone to hallucination, which can irrevocably disrupt automated testing pipelines if an AI agent arbitrarily invents a ROS2 topic or alters a physics parameter with an invalid data type midway through an evaluation.46

The strict JSON-schema enforcement provided by the Model Context Protocol acts as a critical and uncompromising safeguard.50 By defining rigid input constraints and utilizing type-safe capabilities—such as the fastmcp framework automatically generating strict validation schemas directly from Python type hints—the MCP servers act as a robust governance layer.8 They ensure that the non-deterministic intent of the LLM is forcibly translated, validated, and constrained into safe, syntactically flawless commands before they are ever permitted to execute on the underlying engineering software.50

## **Concluding Systemic Implications**

The convergence of FreeCAD, Gazebo, and the Robot Operating System 2 through the Model Context Protocol represents a watershed moment in the field of automated engineering and robotics. By establishing a universal, context-aware transport layer, the persistent silos that have historically separated mechanical CAD design, physical simulation, and robotic middleware control have been systematically dismantled.

Through the combination of freecad-mcp and the RobotCAD workbench, profound geometric manipulation and the generation of complex URDF kinematic chains become fluid, scriptable, and highly automated processes. Through gazebo-mcp, robust physics engines and dynamic, procedurally generated environments can be orchestrated on the fly to provide rigorous testing grounds. Finally, through sophisticated ros2-mcp bridges, the robotic perception and navigational logic are seamlessly connected directly into the cognitive reasoning capabilities of advanced Large Language Models.

The resulting architecture enables a fully autonomous, closed-loop testing environment where an AI agent can conceptualize a mechanism, alter its parametric geometry, simulate its physical reality with extreme fidelity, quantitatively evaluate its performance across randomized edge-case scenarios, and iteratively refine the underlying design based on hard data. As these protocols continue to mature and token-efficient execution patterns become the industry standard, this cyber-physical workflow will fundamentally redefine the speed, accessibility, and precision of robotic systems engineering.

#### **Geciteerd werk**

1. What is Model Context Protocol (MCP)? A guide | Google Cloud, geopend op mei 9, 2026, [https://cloud.google.com/discover/what-is-model-context-protocol](https://cloud.google.com/discover/what-is-model-context-protocol)  
2. Introducing the Model Context Protocol \- Anthropic, geopend op mei 9, 2026, [https://www.anthropic.com/news/model-context-protocol](https://www.anthropic.com/news/model-context-protocol)  
3. What is MCP? MCP Explained in 100 Seconds, geopend op mei 9, 2026, [https://www.youtube.com/watch?v=ttPU0RLOAeI](https://www.youtube.com/watch?v=ttPU0RLOAeI)  
4. Architecture overview \- Model Context Protocol, geopend op mei 9, 2026, [https://modelcontextprotocol.io/docs/learn/architecture](https://modelcontextprotocol.io/docs/learn/architecture)  
5. Understanding MCP servers \- Model Context Protocol, geopend op mei 9, 2026, [https://modelcontextprotocol.io/docs/learn/server-concepts](https://modelcontextprotocol.io/docs/learn/server-concepts)  
6. What Is Model Context Protocol (MCP)? \- Neo4j, geopend op mei 9, 2026, [https://neo4j.com/blog/genai/what-is-model-context-protocol-mcp/](https://neo4j.com/blog/genai/what-is-model-context-protocol-mcp/)  
7. Model Context Protocol, geopend op mei 9, 2026, [https://modelcontextprotocol.io/docs/getting-started/intro](https://modelcontextprotocol.io/docs/getting-started/intro)  
8. Model Context Protocol architecture patterns for multi-agent AI systems \- IBM Developer, geopend op mei 9, 2026, [https://developer.ibm.com/articles/mcp-architecture-patterns-ai-systems/](https://developer.ibm.com/articles/mcp-architecture-patterns-ai-systems/)  
9. Orchestrating Multi-Agent Intelligence: MCP-Driven Patterns in Agent Framework | Microsoft Community Hub, geopend op mei 9, 2026, [https://techcommunity.microsoft.com/blog/azuredevcommunityblog/orchestrating-multi-agent-intelligence-mcp-driven-patterns-in-agent-framework/4462150](https://techcommunity.microsoft.com/blog/azuredevcommunityblog/orchestrating-multi-agent-intelligence-mcp-driven-patterns-in-agent-framework/4462150)  
10. Flexibility to Framework: Building MCP Servers with Controlled Tool Orchestration \- AWS, geopend op mei 9, 2026, [https://aws.amazon.com/blogs/devops/flexibility-to-framework-building-mcp-servers-with-controlled-tool-orchestration/](https://aws.amazon.com/blogs/devops/flexibility-to-framework-building-mcp-servers-with-controlled-tool-orchestration/)  
11. FreeCAD Tools and MCP Server \- LobeHub, geopend op mei 9, 2026, [https://lobehub.com/mcp/spkane-freecad-mcp](https://lobehub.com/mcp/spkane-freecad-mcp)  
12. GitHub \- bonninr/freecad\_mcp: FreecadMCP connects Freecad to Claude AI and other MCP-ready tools like Cursor through the Model Context Protocol (MCP), allowing Claude to directly interact with and control Freecad. This integration enables prompt assisted CAD 3d Design., geopend op mei 9, 2026, [https://github.com/bonninr/freecad\_mcp](https://github.com/bonninr/freecad_mcp)  
13. spkane/freecad-addon-robust-mcp-server \- GitHub, geopend op mei 9, 2026, [https://github.com/spkane/freecad-addon-robust-mcp-server](https://github.com/spkane/freecad-addon-robust-mcp-server)  
14. neka-nat/freecad-mcp \- GitHub, geopend op mei 9, 2026, [https://github.com/neka-nat/freecad-mcp](https://github.com/neka-nat/freecad-mcp)  
15. FreeCAD | Awesome MCP Servers, geopend op mei 9, 2026, [https://mcpservers.org/servers/bonninr/freecad\_mcp](https://mcpservers.org/servers/bonninr/freecad_mcp)  
16. FreeCAD \+ GitHub Copilot MCP \= A New Era of AI‑Driven CAD Workflows \- Reddit, geopend op mei 9, 2026, [https://www.reddit.com/r/FreeCAD/comments/1obo6wt/freecad\_github\_copilot\_mcp\_a\_new\_era\_of\_aidriven/](https://www.reddit.com/r/FreeCAD/comments/1obo6wt/freecad_github_copilot_mcp_a_new_era_of_aidriven/)  
17. A Model Context Protocol (MCP) server that enables AI assistants to interact with FreeCAD for 3D modeling and CAD operations \- GitHub, geopend op mei 9, 2026, [https://github.com/lucygoodchild/freecad-mcp-server](https://github.com/lucygoodchild/freecad-mcp-server)  
18. FreeCAD MCP: AI-Powered 3D Design & Automation, geopend op mei 9, 2026, [https://mcpmarket.com/server/freecad](https://mcpmarket.com/server/freecad)  
19. FreeCAD MCP \- An MCP tool integrated with functions such as 3D model creation and controlled by Claude Desktop, geopend op mei 9, 2026, [https://mcp.aibase.com/server/1916341291153334274](https://mcp.aibase.com/server/1916341291153334274)  
20. FreeCAD & Python | Using the API for automation \- YouTube, geopend op mei 9, 2026, [https://www.youtube.com/watch?v=RQW723n3DkU](https://www.youtube.com/watch?v=RQW723n3DkU)  
21. Automating FreeCAD with Python | Parametric Mugs \- YouTube, geopend op mei 9, 2026, [https://www.youtube.com/watch?v=T\_t6QklsP50](https://www.youtube.com/watch?v=T_t6QklsP50)  
22. Headless FreeCAD, geopend op mei 9, 2026, [https://wiki.freecad.org/Headless\_FreeCAD](https://wiki.freecad.org/Headless_FreeCAD)  
23. TechDraw: Ability to "Headless" export of SVG and PDF · Issue \#5710 \- GitHub, geopend op mei 9, 2026, [https://github.com/FreeCAD/FreeCAD/issues/5710](https://github.com/FreeCAD/FreeCAD/issues/5710)  
24. GitHub \- drfenixion/freecad.robotcad: RobotCAD is a FreeCAD workbench to generate robot description packages for ROS2 (URDF) with launchers to Gazebo and RViz. Includes controllers based on ros2\_controllers and sensors based on Gazebo. With integrated models library and a lot of other tools. In other words CAD → ROS2., geopend op mei 9, 2026, [https://github.com/drfenixion/freecad.robotcad](https://github.com/drfenixion/freecad.robotcad)  
25. freecad.cross/README.md at main · galou/freecad.cross · GitHub, geopend op mei 9, 2026, [https://github.com/galou/freecad.cross/blob/main/README.md](https://github.com/galou/freecad.cross/blob/main/README.md)  
26. Gazebo MCP Server \- LobeHub, geopend op mei 9, 2026, [https://lobehub.com/pl/mcp/yourusername-gazebo-mcp](https://lobehub.com/pl/mcp/yourusername-gazebo-mcp)  
27. Advancing real-world robotics through simulation with Gazebo Ionic \- Intrinsic, geopend op mei 9, 2026, [https://www.intrinsic.ai/blog/posts/advancing-real-world-robotics-through-simulation-with-gazebo-ionic](https://www.intrinsic.ai/blog/posts/advancing-real-world-robotics-through-simulation-with-gazebo-ionic)  
28. Tutorial : Physics Parameters \- Gazebo Classic, geopend op mei 9, 2026, [https://classic.gazebosim.org/tutorials?tut=physics\_params](https://classic.gazebosim.org/tutorials?tut=physics_params)  
29. kvgork/gazebo-mcp: MCP server for Gazebo. Made mostly with Claude \- GitHub, geopend op mei 9, 2026, [https://github.com/kvgork/gazebo-mcp](https://github.com/kvgork/gazebo-mcp)  
30. Why doesn't Gazebo improve its weak physics engine? \- Open Robotics Discourse, geopend op mei 9, 2026, [https://discourse.openrobotics.org/t/why-doesnt-gazebo-improve-its-weak-physics-engine/50320](https://discourse.openrobotics.org/t/why-doesnt-gazebo-improve-its-weak-physics-engine/50320)  
31. Gazebo MCP \- a Hugging Face Space by MCP-1st-Birthday, geopend op mei 9, 2026, [https://huggingface.co/spaces/MCP-1st-Birthday/mcp-gork](https://huggingface.co/spaces/MCP-1st-Birthday/mcp-gork)  
32. LCAS/ros2\_mcp: A comprehensive ROS2 MCP \- GitHub, geopend op mei 9, 2026, [https://github.com/LCAS/ros2\_mcp](https://github.com/LCAS/ros2_mcp)  
33. Robotics\# Gazebo with Ros2. Chapter 1 \- Dilip Kumar, geopend op mei 9, 2026, [https://dilipkumar.medium.com/robotics-gazebo-with-ros2-c6a734ce9634](https://dilipkumar.medium.com/robotics-gazebo-with-ros2-c6a734ce9634)  
34. kakimochi/ros2-mcp-server \- GitHub, geopend op mei 9, 2026, [https://github.com/kakimochi/ros2-mcp-server](https://github.com/kakimochi/ros2-mcp-server)  
35. Talk to Your Robot: A Deep Dive into the kakimochi ROS 2 MCP Server \- Skywork, geopend op mei 9, 2026, [https://skywork.ai/skypage/en/talk-robot-deep-dive-ros2-mcp/1980164713671335936](https://skywork.ai/skypage/en/talk-robot-deep-dive-ros2-mcp/1980164713671335936)  
36. GitHub \- robotmcp/ros-mcp-server: Connect AI models like Claude & GPT with robots using MCP and ROS., geopend op mei 9, 2026, [https://github.com/robotmcp/ros-mcp-server](https://github.com/robotmcp/ros-mcp-server)  
37. WiseVision ROS2: AI Agents & Robotics Bridge \- MCP Market, geopend op mei 9, 2026, [https://mcpmarket.com/server/wisevision-ros2](https://mcpmarket.com/server/wisevision-ros2)  
38. Talk to Your Robot: A Deep Dive into the WiseVision ROS 2 MCP Server \- Skywork, geopend op mei 9, 2026, [https://skywork.ai/skypage/en/talk-robot-wisevision-ros2-mcp-server/1981591269487513600](https://skywork.ai/skypage/en/talk-robot-wisevision-ros2-mcp-server/1981591269487513600)  
39. RoboNeuron: A Middle-Layer Infrastructure for Agent-Driven Orchestration in Embodied AI \- arXiv, geopend op mei 9, 2026, [https://arxiv.org/html/2512.10394](https://arxiv.org/html/2512.10394)  
40. wise-vision/ros2\_mcp: Advanced MCP Server ROS 2 bridging AI agents straight into robotics \- GitHub, geopend op mei 9, 2026, [https://github.com/wise-vision/ros2\_mcp](https://github.com/wise-vision/ros2_mcp)  
41. ROS 2 Bridge MCP Server by Nicolas Gres: Your Dynamic Gateway to AI-Powered Robotics \- Skywork, geopend op mei 9, 2026, [https://skywork.ai/skypage/en/ros-2-bridge-ai-robotics/1981228555226238976](https://skywork.ai/skypage/en/ros-2-bridge-ai-robotics/1981228555226238976)  
42. Robotic Software Testing: ROS2, Gazebo, and Motion Planning Validation \- Testriq, geopend op mei 9, 2026, [https://www.testriq.com/blog/post/robotic-software-testing-ros2-gazebo-and-motion-planning-validation](https://www.testriq.com/blog/post/robotic-software-testing-ros2-gazebo-and-motion-planning-validation)  
43. jackvice/RoboTerrain: Gymnasium and ROS2 control with gazebo. \- GitHub, geopend op mei 9, 2026, [https://github.com/jackvice/RoboTerrain](https://github.com/jackvice/RoboTerrain)  
44. Bachelor thesis report \- Diva-portal.org, geopend op mei 9, 2026, [https://www.diva-portal.org/smash/get/diva2:1768198/FULLTEXT02](https://www.diva-portal.org/smash/get/diva2:1768198/FULLTEXT02)  
45. Benchmarking Full-Stack ROS 2 Simulation Platforms for Mobile Robots \- IEEE Xplore, geopend op mei 9, 2026, [https://ieeexplore.ieee.org/iel8/7083369/11435997/11457326.pdf](https://ieeexplore.ieee.org/iel8/7083369/11435997/11457326.pdf)  
46. Architecture patterns for using Model Context Protocol (MCP) in Android UI Automation/Testing : r/androiddev \- Reddit, geopend op mei 9, 2026, [https://www.reddit.com/r/androiddev/comments/1qjucqd/architecture\_patterns\_for\_using\_model\_context/](https://www.reddit.com/r/androiddev/comments/1qjucqd/architecture_patterns_for_using_model_context/)  
47. Modelling and Model-Checking a ROS2 Multi-Robot System using Timed Rebeca \- arXiv, geopend op mei 9, 2026, [https://arxiv.org/html/2511.15227v1](https://arxiv.org/html/2511.15227v1)  
48. POLITECNICO DI TORINO, geopend op mei 9, 2026, [https://webthesis.biblio.polito.it/36530/1/tesi.pdf](https://webthesis.biblio.polito.it/36530/1/tesi.pdf)  
49. Code execution with MCP: building more efficient AI agents \- Anthropic, geopend op mei 9, 2026, [https://www.anthropic.com/engineering/code-execution-with-mcp](https://www.anthropic.com/engineering/code-execution-with-mcp)  
50. What is the Model Context Protocol (MCP)? \- Databricks, geopend op mei 9, 2026, [https://www.databricks.com/blog/what-is-model-context-protocol](https://www.databricks.com/blog/what-is-model-context-protocol)  
51. jango-blockchained/mcp-freecad \- GitHub, geopend op mei 9, 2026, [https://github.com/jango-blockchained/mcp-freecad](https://github.com/jango-blockchained/mcp-freecad)

[image1]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACsAAAAXCAYAAACS5bYWAAACL0lEQVR4Xu2WPWhUQRSFTzCKIf6QHwIJSkBUFMUoWstKgiKSFJJCsLTQQpRERAgYiCIiokUKA4mFEgIBBQsRtZEl6bRO40+RFIKFiEKagJpz9s7bnZ3dfftcyULgHfiKd2f2zXn33plZIFWq9att5CqZJGNkL2komrH22kDOkEdknJwkG4tmUD0kSzKkhVwkK+Qa6md4C3lGLpMOcox8Ia/Jdm9e7it+kwH3LMMfyHeyP5q0xuojf8gEaXSxO+QvLHl5PXDBC+55K5knv2BZj1Oro5JU2i5Ur1AGlrAXZLOLjcB8DbvnnNQX7bAXSwfJD1hrqDxx2gNboDscgL13lFxHdbMab0PBaBN5Q5bJ0WhSKG20GbJEDgdjlaR5b1Fs+F+MhlLSVGUZVQ+X/L6ZzMJMqrFPoZDpJPIN/4/RSzAP38hNmK9Y7SNfyTQSTPYkw+/IFGoz6ktt8ByWuNhNrkXUCmpufWlSKaM6WbTArmCsFp2FeXgJM59bYMjhH8A3YBOferE46bf3YRk9QF6h/KarpF7ykOzwYtpY6ttF0ukHwl0nkzKrTFWTbzQqvUqX1LBOnCxsPSUpUr+LLcBOK+wkn8gTFG4KHSHvYcfXERerJBm9B6tM2KNJDesSeEw+kkMupnfdhpnVTZrXafIZtug52AI/XbyaTpArKDUaaTe5RTaFA4FU/jnYlXue3IVd+fJU8v9Ah3GGDJLjKDOhDtJRqWrIg1pAFU6VKlU9tAooml6yP8u/iwAAAABJRU5ErkJggg==>

[image2]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACwAAAAXCAYAAABwOa1vAAACY0lEQVR4Xu2WTUgWQRzGn6jAqIwwkD6glCi8BVESWHTokEgRFSIVdErBS+AhIZAO4qVDkAeFiKJDVNJVETpU1CHo5MEEQfqgDwr0pEEG5fP433FnZ5tV3ksR+4Mf77uzM7vPvjM7/xcoKfl/WEfP0Vv0Ot2bPV1INb0MG9tDt2ZPL7KKNtJ+OkBb6OpMjxS1t9JdQfsSm+gT2ks30H30DT3jd4qwk47RS7SKNtNJetDro7BX6HNaR2vofdgDrk366Ac7QW/ST3SO7k/O5eimr+lmr+08naC1XlvIGnqbPk6+O/roKCyE0I2/0qalHkA9fU+PJ8fqq4c9Sq+hILBCKuy9oP0AnaUng3Yf3fQL7IF9TiN7Qz2AwvlLZSN9Qe/CZsBH14sGbqDTyAdWZw3SzWIco7+QD6yp/Q2bJS2TYeQDa+k9Q35mRWFgFywWOGz3ccFigdXugsUCh+2iMLC7eBhsJYF14eUCK4xChcEqDqyFXmngLiwfWC/tFPLBKg4cCxZr9/krS8K96WEwF/hq0O6jbeon4oG1W2i707YXBnOBtVNox/ApDOwG6k3WG+3QDjCffDpUYLYh3Ya203ew6uXTAdt5tAMJBfCPxRY6jvxYURhYXKAfYFVIKJCq3itYSFEDq2g/6KGCfqpcQ/QB0mKym36kbcmxOEy/Ib2WjwJ/h9WCP6KbDNKn9BQshJ5eJdqhmRiBlV2VY4eCqv0R7AW+Q1/SHV4foeXxlrbTi7DS34l0ttbTh3QGtpycn+mNpE8GDdxDz9IjSGv8StCfFU2fxuoz9qdGs6T1LfW9pKTkX2cBJ6aYxBCd5oIAAAAASUVORK5CYII=>

[image3]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAADYAAAAXCAYAAABAtbxOAAAC1UlEQVR4Xu2WTchNQRjHH6HIV3qVfOUzslDKV4SVRCJhIZQVSkpZUEopWbC0oCSykMj2lSJesRArCkXykRJlxwL5+P/Mnfc8d86Ze3QXbxbnX7/uvXPnzDz/mTnPPGaNGjUaKA0X28RZcVLMaf+7o0aL/RaePSImtP/9V4PEEnFKnBbrxOC2HkHMy/yMRTzEldM0sTVt9BojbopjYqSYL56Jzb5TRlPFY7FLDBNrxQux2PXB1EFxV0wXPeKSheCHun7Mx7zMTxzEQ1zEFzVX7BW3xU9x0f1X0iHxSIx1bdvFczHetaUaIs6Ja63vUcfFDStWe4H4KJb39zCbId6KNa3fU8RLC/NGEQ9x7XNtGNsolon31sFYfDjtsEh8ERuSdi+C+2BhYbw2ia8WDCGMYsIf0VHinrhgYUcx5J9BtLOzfRZ20IuxGDONu1+swGcrd2ACJiKonFaJX1Y2tl78thAsx7PXysYItM+Kk8K7lxpDxMXisYhetcaigbRDrt0rGsgZoz0ayBmL7cyTM1bVXmssBpF2+BdjBF5nLAbQydis1vcqA10bI4t1a+yA1Rsj+byyzsZmiltWbaBrYzkDuXav//ooxsyWdojGDiftXqTvH5Y3RnbkGuA6yBkjM5IhSVJVBoiLtD45aa81FifotZDBosh431ufUVyUEy2kYTRJvLGQ0bz2WMi0ZFyEcf8bjRNPrXiWa4UL188XM2oaG6o1hnaIdxaqAkTg3PoPrLj1eyxUGN/E0g79qCSuistWXNokB1bdlz8rxCcrxmL8h+Jo7GDVz0VFY9xzcaFLIpgz4o6FW51gWU1Kmyh29rqFcokyKgpDtF+xkIjOi/tWPjocy9dit9hpoXSiNPJBLWz1ofzaYuGOO2HtZRc7ill2l+MOFBJPxDzXr19MMNvCgCutfbA6UczybvAsn1XFLWJXeP+A71UaIVZbWGDKrEaNGjUaOP0B9UrBknZxWRgAAAAASUVORK5CYII=>

[image4]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAADsAAAAYCAYAAABEHYUrAAACa0lEQVR4Xu2Wz6tNURTHv0L5USQ/3hMGZED5McB7vXoTMiBlIEkxeDMTpRRloM7UBEUpTLyJSM+AAaZKMjLiH5B6vSRlwgDf79t7v7v2urd7zzlyS+1Pfevevfbdt7XX+q5zgEKh8D9zkJqjfke9oJab+CrqlYlLM9RKs+dfs456jc7/f6V2ZTuAyzGW9Inane2ILKLuUj+pH9REHp7nBPUE+UUMm+PUN4Rkqjw0z2bqDbXDByxrqAfUBYSDbiNcgOUidcatDZuKOodQtY/USBYF9lL3qCVuPUObblAbEQ7RYVtNXD/WIdpXhy3UUr9okAXW+sUB6Df3EaqnYqgop7MdoRgqSl+0STcmKoSDzi9Eg2dUeXVAHc5SN9E74VHqKTXmAwPYhnDhyxBsJrv5+XKNmjTfe3Kd2hc/y/gaAG+p1XFNB9yKn+sgC+iy9BubcNtExVHqUvysBJWonS8qxCN0t3ZG8quqJ9SyD6lf1JG4pqo39atP+G8SFRV12HxXC9v5spO6g5p+tQNJSSpZJa1bbOJXS0r4MfUM7RNNft1k1lRBO18a+zWh9lUbq50PoZlfPRpW76lp9PZwHaxfLRU682WgX3XzarMDPoAwZHTQB4SD2qDJKW+NU1Po9nBd9Hy94hfJduozNUu9RMeKPfF+taQ2UcJN/SpSoql1dbFTaJdwhdyvCZ2ZHkN64enrV7WoXv1W+ECkor4gmL8JSvQ5td+tt0l4PULV9vhAJD2GvBUX0C19R+c9UpuPZTsCegypOk39ehXdiSaU8EnqlA84NlDvkL/v6pIW200IA1QF6+vXQqFQKBSGxB/OY3eLVpg/TgAAAABJRU5ErkJggg==>

[image5]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACsAAAAXCAYAAACS5bYWAAACXUlEQVR4Xu2WS6iNURiGX0kRjkRKkZJcipDLUGcgMiB1DESZGJCUJBQTkgFFMjQ6ksLESAmDU2fGSMnAJZEohQgDcnkf31r2f9n772wyoP3W097r+9e/vnfdf6mnnv5d9Zk95pw5auaaUaUaoXnmpKLeFjOu8Gyn+WC+F3hjVptF5kHl2Vdz2Yzn5ZFqsRky/Way2WE+m30qGx4w980SM8EcMzfNpEIddEhh5mAljlaYT4p8tNG1zip6uSGVMXzHvDYLUmymeWi2pjLK9XYXYgiTmF1fiaNl5qP+wOwpRePbU3miGTbvFaOOMEkSkmUx6hdVT/xXzY4xU83oVF5o3qrcIKNfNYvOm5dmdiHWrVnyTjPT29DYITYao/VMsTazMNXJbDWezW5TPfla1dcs8afmnmLjwl2VZ7skduQlhcnHikbzSNPokOqmUJPZG2olz1w131Q2y7vX1dqoKxUze0Ux642ab16YC4pOwC3VTaEmsyNdBsROp/8YJhfH3KwUa1TeOCTk7ETtTHWKd2uW0WNAyHtG5ZOpJCruTRSHPCfEDDquuinE8+dmRiHWrdmsAfPFHFAY5yI5XKyQX64awQQJOQUQPaXH3EZZY821BP+zfscsU87UswTy2iXn/l811DrsB9WqNMXcVizypZXYkVRGcxSjurkQQ003WDuz/LIZ2Se0ibjGBxW3aUnrzCNzQpGYkXqX4kUtN08U07RJcXvxTl4+rO9XCqMZDORvA44mZic/4zuCE2hXKnPFcxpB/sZoNzs/p7FfYWKVOh8ZbIQ1ZqNiVnrqqaf/QT8A8l+ug2yvNy0AAAAASUVORK5CYII=>