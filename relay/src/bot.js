function commandList({ SlashCommandBuilder, PermissionFlagsBits }) {
  return [
    new SlashCommandBuilder()
      .setName('pair')
      .setDescription('Pair a channel with a Raspi Host game server and get a pairing code')
      .addChannelOption((option) => option
        .setName('channel')
        .setDescription('The channel game chat should be bridged to')
        .setRequired(true))
      .addStringOption((option) => option
        .setName('label')
        .setDescription('Optional note, e.g. the server name'))
      .setDefaultMemberPermissions(PermissionFlagsBits.ManageGuild)
      .toJSON(),
    new SlashCommandBuilder()
      .setName('unpair')
      .setDescription('Remove the pairing of a channel and invalidate its code')
      .addChannelOption((option) => option
        .setName('channel')
        .setDescription('The channel to unpair')
        .setRequired(true))
      .setDefaultMemberPermissions(PermissionFlagsBits.ManageGuild)
      .toJSON(),
    new SlashCommandBuilder()
      .setName('relay-status')
      .setDescription('Show which channels of this server are paired')
      .setDefaultMemberPermissions(PermissionFlagsBits.ManageGuild)
      .toJSON(),
  ];
}

/**
 * Discord side of the bridge.
 *
 * Bot messages and webhooks are ignored when relaying, which is what stops the
 * game -> Discord -> game feedback loop.
 */
export async function createBot({ core, token, log = () => {}, onChange = () => {} }) {
  // Imported lazily so a missing `npm install` or a missing token produces a
  // clear message from index.js instead of a module-resolution stack trace.
  const {
    Client, Events, GatewayIntentBits, PermissionFlagsBits, SlashCommandBuilder,
  } = await import('discord.js');

  const client = new Client({
    intents: [
      GatewayIntentBits.Guilds,
      GatewayIntentBits.GuildMessages,
      GatewayIntentBits.MessageContent,
    ],
  });

  client.once(Events.ClientReady, async (readyClient) => {
    log('bot', `logged in as ${readyClient.user.tag}`);
    try {
      // Global commands: propagation can take a while. Set DISCORD_GUILD_ID to
      // register in one guild instead, which is instant while testing.
      if (process.env.DISCORD_GUILD_ID) {
        await readyClient.application.commands.set(commandList({ SlashCommandBuilder, PermissionFlagsBits }), process.env.DISCORD_GUILD_ID);
      } else {
        await readyClient.application.commands.set(commandList({ SlashCommandBuilder, PermissionFlagsBits }));
      }
      log('bot', 'slash commands registered');
    } catch (error) {
      log('error', `failed to register slash commands: ${error.message}`);
    }
  });

  client.on(Events.MessageCreate, (message) => {
    if (message.author.bot || message.webhookId || !message.guildId) {
      return;
    }
    if (!core.pairingForChannel(message.channelId)) {
      return;
    }
    const name = message.member?.displayName ?? message.author.username;
    core.publish(message.channelId, {
      name,
      message: message.content,
      authorId: message.author.id,
    });
  });

  client.on(Events.InteractionCreate, async (interaction) => {
    if (!interaction.isChatInputCommand() || !interaction.inGuild()) {
      return;
    }
    const isManager = interaction.memberPermissions?.has(PermissionFlagsBits.ManageGuild);
    if (!isManager) {
      await interaction.reply({ content: 'You need the "Manage Server" permission to do that.', ephemeral: true });
      return;
    }

    try {
      if (interaction.commandName === 'pair') {
        const channel = interaction.options.getChannel('channel', true);
        const label = interaction.options.getString('label') ?? '';
        const { code } = core.createPairing({
          guildId: interaction.guildId,
          channelId: channel.id,
          label,
        });
        onChange();
        log('pair', `channel ${channel.id} (${interaction.guild.name}) paired`);
        await interaction.reply({
          ephemeral: true,
          content: [
            `Paired <#${channel.id}>. Add these two lines to that server's \`RaspiConfig.toml\`:`,
            '',
            '```toml',
            '[Discord]',
            'Enabled = true',
            'RelayUrl = "https://relay.example.com"',
            `PairingCode = "${code}"`,
            '```',
            'The code is a secret: anyone who has it can read and write this channel\'s bridge. ',
            'Use `/unpair` to revoke it.',
          ].join('\n'),
        });
        return;
      }

      if (interaction.commandName === 'unpair') {
        const channel = interaction.options.getChannel('channel', true);
        const removed = core.revokeByChannel(channel.id);
        if (removed) {
          onChange();
        }
        await interaction.reply({
          ephemeral: true,
          content: removed ? `Unpaired <#${channel.id}>; its code no longer works.` : 'That channel is not paired.',
        });
        return;
      }

      if (interaction.commandName === 'relay-status') {
        const paired = [...core.pairings.values()]
          .filter((pairing) => pairing.guildId === interaction.guildId)
          .map((pairing) => `• <#${pairing.channelId}> ${pairing.label ? `(${pairing.label})` : ''}`);
        await interaction.reply({
          ephemeral: true,
          content: paired.length > 0 ? `Paired channels:\n${paired.join('\n')}` : 'No channels are paired.',
        });
      }
    } catch (error) {
      log('error', `interaction failed: ${error.message}`);
      if (!interaction.replied) {
        await interaction.reply({ content: 'Something went wrong, check the relay logs.', ephemeral: true });
      }
    }
  });

  return {
    client,
    async start() {
      await client.login(token);
    },
    async stop() {
      await client.destroy();
    },
  };
}

/**
 * Sends a game message into the paired Discord channel.
 * Mention parsing is disabled so player text can never ping @everyone.
 */
export function createDiscordSender({ client, log = () => {} }) {
  return async function sendToDiscord(pairing, { name, message }) {
    const channel = await client.channels.fetch(pairing.channelId);
    if (!channel || !channel.isTextBased()) {
      throw new Error(`channel ${pairing.channelId} is not text based`);
    }
    const safeName = name.replace(/\*\*/g, '').replace(/[`_~]/g, '');
    await channel.send({
      content: `**${safeName}**: ${message}`,
      allowedMentions: { parse: [] },
    });
    log('relay', `delivered message from ${safeName}`);
  };
}
